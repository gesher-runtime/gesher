#!/usr/bin/env python3
"""Regression gates for automatic HWUI GPU selection and Ganesh qualification.

Also the OpenGL ES version declared for the default EGL display
(compat/bionic/egl_gles_version.h): ES 3.0 only on a hardware Direct3D 11
device, with a negative control that must fail.

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
        for name, sources, defines, passes in (
            ("policy", ["tests/native/hwui_renderer_policy_test.cc"], [], True),
            ("selector", ["compat/hwui/gl/pipeline_select.cc",
                          "tests/hwui-renderer/selector_test.cc"], [], True),
            ("gles-version", ["tests/native/egl_gles_version_test.cc"], [], True),
            # Negative control: ES 3.0 declared on WARP must be caught.
            ("gles-version-negative", ["tests/native/egl_gles_version_test.cc"],
             ["-DGESHER_EGL_VERSION_NEGATIVE_CONTROL"], False),
        ):
            exe = str(Path(tmp) / (name + (".exe" if os.name == "nt" else "")))
            cmd = compiler + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                              "-fno-exceptions", *defines,
                              "-I", str(ROOT / "tests/hwui-renderer/stubs"),
                              "-I", str(ROOT), *(str(ROOT / src) for src in sources),
                              "-o", exe]
            if args.sanitize:
                cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
            subprocess.run(cmd, cwd=ROOT, check=True, timeout=120)
            # The negative control's own FAIL lines are expected; keep them out of the log.
            run = subprocess.run([exe], cwd=ROOT, timeout=30, capture_output=not passes)
            if (run.returncode == 0) != passes:
                raise SystemExit(f"{name}: exit {run.returncode}, expected "
                                 + ("success" if passes else "failure (negative control)"))
    probe = [sys.executable, str(ROOT / "tools/test_ganesh_probe.py"),
             "--cxx", args.cxx]
    if args.sanitize:
        probe.append("--sanitize")
    subprocess.run(probe, cwd=ROOT, check=True, timeout=180)
    trim = [sys.executable, str(ROOT / "tools/test_hwui_trim.py"), "--cxx", args.cxx]
    if args.sanitize:
        trim.append("--sanitize")
    subprocess.run(trim, cwd=ROOT, check=True, timeout=180)
    print("hwui GPU policy, selector, EGL ES version and actual Ganesh preflight all passed")

if __name__ == "__main__":
    main()
