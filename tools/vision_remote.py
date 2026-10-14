#!/usr/bin/env python3
"""Run a local script in WSL over an already authenticated SSH connection."""
import argparse
import base64
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--socket", required=True, help="Authenticated SSH control socket")
p.add_argument("--host", default="xenolith-lnl")
p.add_argument("--distribution", default="Ubuntu-24.04")
p.add_argument("--python", action="store_true")
p.add_argument("script", nargs="?")
a = p.parse_args()
source = open(a.script, "rb").read() if a.script else sys.stdin.buffer.read()
if not a.python:
    source = ("import subprocess\nraise SystemExit(subprocess.run(['bash','-s'],input="
              + repr(source) + ").returncode)\n").encode()
encoded = base64.b64encode(source).decode()
remote = (f'wsl.exe -d {a.distribution} -- python3 -c '
          f'"import base64;exec(base64.b64decode(\'{encoded}\'))"')
raise SystemExit(subprocess.run(["ssh", "-o", "BatchMode=yes", "-S", a.socket,
                                a.host, remote]).returncode)
