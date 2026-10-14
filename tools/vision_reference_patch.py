#!/usr/bin/env python3
"""Align the isolated CPU vision oracle's GELU precision with F32 intermediates.

Apply only to a separate copy of the pinned llama.cpp sources. Language-model
comparisons keep the original CPU library, including its FP16 GELU tables.
"""
import argparse
import hashlib
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
a = p.parse_args()
file = a.source / 'ggml/src/ggml-cpu/vec.h'
original = file.read_bytes()
old = '885a50e405f20f75ea70cb7fb062934812beffc9229ddb56a003aa150163d2fa'
new = '6b26b2d205a08347db270681cf0fa9494e581f03d58bf38101fe9db603873ff5'
digest = hashlib.sha256(original).hexdigest()
if digest == new:
    print('F32 GELU adapter already applied')
elif digest != old:
    raise SystemExit('pinned vec.h checksum mismatch')
else:
    changed = original.replace(b'#define GGML_GELU_FP16\n', b'/* Vision oracle: evaluate GELU in F32, without the FP16 LUT shortcut. */\n')
    assert hashlib.sha256(changed).hexdigest() == new
    file.write_bytes(changed)
    print('F32 GELU adapter applied', new)
