#!/usr/bin/env python3
"""Regression gates for automatic HWUI GPU selection and Ganesh qualification.

The real C++ production selector is compiled against test-only adapters.
The probe suite additionally extracts the production ANGLE/Skia qualification
function, reproducing MakeGL failures and verifying EGL cleanup/fallback.
Neither fixture replaces real Windows GPU and memory testing.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    compiler = shlex.split(args.cxx)
    with tempfile.TemporaryDirectory(prefix="gesher-hwui-") as tmp:
        for name, sources in (
            ("policy", ["tests/native/hwui_renderer_policy_test.cc"]),
            ("selector", ["compat/hwui/gl/pipeline_select.cc",
                          "tests/hwui-renderer/selector_test.cc"]),
        ):
            exe = str(Path(tmp) / (name + (".exe" if os.name == "nt" else "")))
            cmd = compiler + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                              "-fno-exceptions",
                              "-I", str(ROOT / "tests/hwui-renderer/stubs"),
                              "-I", str(ROOT), *(str(ROOT / src) for src in sources),
                              "-o", exe]
            if args.sanitize:
                cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
            subprocess.run(cmd, cwd=ROOT, check=True, timeout=120)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)
    probe = [sys.executable, str(ROOT / "tools/test_ganesh_probe.py"),
             "--cxx", args.cxx]
    if args.sanitize:
        probe.append("--sanitize")
    subprocess.run(probe, cwd=ROOT, check=True, timeout=180)
    trim = [sys.executable, str(ROOT / "tools/test_hwui_trim.py"), "--cxx", args.cxx]
    if args.sanitize:
        trim.append("--sanitize")
    subprocess.run(trim, cwd=ROOT, check=True, timeout=180)
    print("hwui GPU policy, selector and actual Ganesh preflight all passed")

if __name__ == "__main__":
    main()
