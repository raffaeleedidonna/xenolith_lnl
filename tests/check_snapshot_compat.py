#!/usr/bin/env python3
"""Cross-build snapshot checks using the real engine reader and writer.

By default host session buffers replace GPU allocation. Full mode
extracts the historical writer from Git (or --legacy-source), rather than making
the new writer imitate the old header. --model also validates real inference in
separate writer/reader processes on the GPU. All artifacts live in a temporary tree.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

BASELINE = "68b8fbf55787eda392c8cbfa32bab5ad14622e8a"
ROOT = Path(__file__).resolve().parent.parent


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def source_hash(root):
    entries = "".join(
        f"{hashlib.sha256((root / name).read_bytes()).hexdigest()}  {name}\n"
        for name in ("xenolith.c", "xenolith.cl")
    )
    return hashlib.sha256(entries.encode()).hexdigest()


def crc64(data):
    value = 0
    for byte in data:
        value ^= byte << 56
        for _ in range(8):
            value = ((value << 1) ^ (0x42F0E1EBA9EA3693 if value >> 63 else 0))
            value &= (1 << 64) - 1
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--current-only", action="store_true")
    parser.add_argument("--legacy-source", type=Path)
    parser.add_argument("--model", type=Path, help="also check GPU inference continuation")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--base-cflags", default="-O3 -march=native -std=c11 -Wall -Wextra -pthread")
    args = parser.parse_args()
    if args.model and (args.current_only or not args.model.is_file()):
        parser.error("--model needs an existing model file and full compatibility checks")
    flags_hash = hashlib.sha256(args.base_cflags.encode()).hexdigest()
    gpu_object = ROOT / "xenolith_gpu_spv.o"
    if not gpu_object.is_file():
        parser.error("build xenolith_gpu_spv.o first (or use the Makefile targets)")

    with tempfile.TemporaryDirectory(prefix="xenolith-snapshot-compat-") as tmp:
        tmp = Path(tmp)

        def compile_fixture(name, source, commit, numeric_hash, cflags_hash, compiler=None,
                            fixture="snapshot_compat_fixture.c", kernel=gpu_object):
            output = tmp / name
            defines = {
                "SNAPSHOT_ENGINE_SOURCE": str(source / "xenolith.c"),
                "XE_BUILD_COMMIT": commit,
                "XE_NUMERIC_SOURCE_HASH": numeric_hash,
                "XE_NUMERIC_CFLAGS_HASH": cflags_hash,
            }
            command = shlex.split(args.cc) + shlex.split(args.base_cflags)
            command += ["-ffunction-sections", "-fdata-sections"]
            command += [f'-D{key}="{value}"' for key, value in defines.items()]
            if compiler:
                command += ["-U__VERSION__", f'-D__VERSION__="{compiler}"']
            command += [ROOT / "tests" / fixture, source / "format.c"]
            if (source / "vision.c").is_file():
                command += [source / "vision.c", "-ldl"]
            command += [kernel, "-Wl,--gc-sections", "-lm", "-lze_loader", "-o", output]
            run(command)
            return output

        def exercise(binary, mode, path, want="ok", capacity=129, tokens=17, state="unchanged"):
            run([binary, mode, path, want, capacity, tokens, state])

        a = compile_fixture("current-a", ROOT, "build-a", source_hash(ROOT), flags_hash)
        # Different diagnostic/compiler/flag identities with unchanged numerical
        # implementation. This is not a test of a second physical compiler.
        b = compile_fixture("current-b", ROOT, "build-b", "different-source",
                            "different-flags", "compat-test-compiler")
        snapshot = tmp / "current.xkv"
        exercise(a, "write", snapshot)
        original = snapshot.read_bytes()
        assert struct.unpack_from("<H", original, 8)[0] == 2
        assert original[328:344].split(b"\0")[0] == b"build-a"
        exercise(b, "read", snapshot)
        exercise(b, "write", tmp / "other.xkv")
        exercise(a, "read", tmp / "other.xkv")
        other = (tmp / "other.xkv").read_bytes()
        assert other[344:376].split(b"\0")[0] == b"compat-test-compiler"
        assert original[256:288] == other[256:288]

        def changed(data, offset, replacement, want, state="unchanged", fix_crc=True):
            copy = bytearray(data)
            copy[offset:offset + len(replacement)] = replacement
            if fix_crc:
                struct.pack_into("<Q", copy, 376, crc64(copy[:376]))
            path = tmp / "changed.xkv"
            path.write_bytes(copy)
            exercise(a, "read", path, want, state=state)

        for i, kind in enumerate(("model", "tokenizer", "template", "layout", "context", "numeric")):
            offset = 96 + i * 32
            changed(original, offset, bytes([original[offset] ^ 1]), f"{kind}-mismatch")
        abi = struct.unpack_from("<I", original, 320)[0]
        changed(original, 320, struct.pack("<I", abi + 1), "numeric-mismatch")
        changed(original, 8, struct.pack("<H", 99), "version")
        changed(original, 10, struct.pack("<H", 1), "version")
        changed(original, 324, b"\1", "format")
        changed(original, 343, b"x", "format")
        changed(original, 375, b"x", "format")
        changed(original, 328, b"new-origin", "ok")
        changed(original, 328, b"new-origin", "format", fix_crc=False)
        exercise(a, "read", snapshot, "context-mismatch", capacity=130)
        # Payload corruption must leave no partially restored session usable.
        global_offset = struct.unpack_from("<Q", original, 384 + 4 * 40 + 8)[0]
        changed(original, global_offset, bytes([original[global_offset] ^ 1]),
                "format", state="empty")
        print("snapshot compatibility: cross-build v2, rejection checks and corruption PASS", flush=True)

        # Global rows and SWA wraparound at a non-power-of-two context capacity.
        large = tmp / "wrapped.xkv"
        exercise(a, "write", large, capacity=2049, tokens=1057)
        exercise(b, "read", large, capacity=2049, tokens=1057)
        large.unlink()
        print("snapshot compatibility: exact wrapped SWA/global state PASS", flush=True)

        if not args.current_only:
            legacy = tmp / "legacy"
            legacy.mkdir()
            for name in ("xenolith.c", "xenolith.cl", "xenolith.h", "xenolith_internal.h",
                         "format.c", "format.h"):
                if args.legacy_source:
                    data = (args.legacy_source / name).read_bytes()
                else:
                    data = run(["git", "show", f"{BASELINE}:{name}"], cwd=ROOT,
                               stdout=subprocess.PIPE).stdout
                (legacy / name).write_bytes(data)
            if source_hash(legacy) != "528b6db377397368158a28dc155047628ac30a5a06c38aa6c32a5680ccd56d84":
                parser.error("legacy source does not match the supported baseline")
            kernel_changed = (legacy / "xenolith.cl").read_bytes() != (ROOT / "xenolith.cl").read_bytes()
            legacy_kernel = gpu_object
            if kernel_changed:
                # Never impersonate the old producer with today's compiled
                # kernel. The opaque v1 identity must reject this migration.
                run(["ocloc", "compile", "-file", "xenolith.cl", "-device", "xe-lp",
                     "-spv_only", "-output", "xenolith_gpu", "-output_no_suffix",
                     "-out_dir", ".", "-options", "-cl-std=CL3.0", "-q"], cwd=legacy)
                run(["ld", "-r", "-b", "binary", "-o", "xenolith_gpu_spv.o", "xenolith_gpu.spv"], cwd=legacy)
                run(["objcopy", "--rename-section", ".data=.rodata,alloc,load,readonly,data,contents",
                     "xenolith_gpu_spv.o"], cwd=legacy)
                legacy_kernel = legacy / "xenolith_gpu_spv.o"
            old = compile_fixture("legacy-writer", legacy, BASELINE[:9], source_hash(legacy), flags_hash,
                                  kernel=legacy_kernel)
            saved = tmp / "legacy.xkv"
            exercise(old, "write", saved)
            legacy_bytes = saved.read_bytes()
            assert struct.unpack_from("<H", legacy_bytes, 8)[0] == 1
            exercise(old, "read", saved)
            exercise(a, "read", saved, "numeric-mismatch" if kernel_changed else "ok")
            assert saved.read_bytes() == legacy_bytes  # Read-only compatibility.
            exercise(b, "read", saved, "numeric-mismatch")
            exercise(old, "read", snapshot, "version")
            changed(legacy_bytes, 256, bytes([legacy_bytes[256] ^ 1]), "numeric-mismatch")
            changed(legacy_bytes, 256, original[256:288], "numeric-mismatch")
            for i, kind in enumerate(("model", "tokenizer", "template", "layout", "context")):
                offset = 96 + i * 32
                changed(legacy_bytes, offset, bytes([legacy_bytes[offset] ^ 1]), f"{kind}-mismatch")
            exercise(old, "write", large, capacity=2049, tokens=1057)
            exercise(a, "read", large, "numeric-mismatch" if kernel_changed else "ok", capacity=2049, tokens=1057)
            large.unlink()
            print(f"snapshot compatibility: real v1 writer {BASELINE[:9]} -> v2 "
                  f"{'safe rejection (kernel changed)' if kernel_changed else 'reader'} PASS", flush=True)
            if args.model:
                old_model = compile_fixture("legacy-model", legacy, BASELINE[:9],
                                            source_hash(legacy), flags_hash,
                                            fixture="snapshot_compat_model.c", kernel=legacy_kernel)
                new_model = compile_fixture("current-model", ROOT, "build-a", source_hash(ROOT),
                                            flags_hash, fixture="snapshot_compat_model.c")
                other_model = compile_fixture("other-model", ROOT, "build-b", "different-source",
                                              "different-flags", "compat-test-compiler",
                                              fixture="snapshot_compat_model.c")
                for label, writer, reader in (("v1 -> v2", old_model, new_model),
                                               ("v2 cross-build", new_model, other_model)):
                    artifacts = tmp / "model-snapshots"
                    artifacts.mkdir()
                    run([writer, "write", args.model.resolve(), artifacts])
                    mode = "replay" if writer == old_model and kernel_changed else "read"
                    run([reader, mode, args.model.resolve(), artifacts])
                    for artifact in artifacts.iterdir():
                        artifact.unlink()
                    artifacts.rmdir()
                    print(f"snapshot model compatibility: {label}, {mode}, exact continuation PASS",
                          flush=True)
    if not args.model:
        print("Host buffers only; model continuation still requires GPU/model validation.")


if __name__ == "__main__":
    main()
