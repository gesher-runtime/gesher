#!/usr/bin/env python3
"""Compile actual GLES readback layout and host display pacing contracts.

No AOSP checkout or Windows machine is required. A separate Windows test
exercises the live ImageReader/SurfaceTexture/ANGLE pipeline and its fallback.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-egl-readback-") as tmp:
        exe = Path(tmp) / ("readback_test.exe" if os.name == "nt" else "readback_test")
        flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-fno-exceptions", "-I", str(ROOT),
                 str(ROOT / "tests/native/egl_readback_layout_test.cc"),
                 "-o", str(exe)]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer"]
        subprocess.run(shlex.split(args.cxx) + flags, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        result = subprocess.run([str(exe)], cwd=ROOT, env=env, timeout=120,
                                check=False).returncode
        if result:
            return result
        pacing = [sys.executable, str(ROOT / 'tools/test_display_refresh_contracts.py'),
                  '--cxx', args.cxx]
        if args.sanitize:
            pacing += ['--sanitize']
        return subprocess.run(pacing, cwd=ROOT, env=env, timeout=150, check=False).returncode

if __name__ == "__main__":
    raise SystemExit(main())
