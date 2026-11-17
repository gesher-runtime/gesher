#!/usr/bin/env python3
"""Reproduce the real ANGLE Ganesh preflight with controlled EGL/Skia failures.

Unlike a policy-only check, the function under test is extracted verbatim
from the production angle_gl.cc. AOSP libraries are substituted only at the
boundary; full Windows/ANGLE CI and Intel iGPU tests are still required.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "compat/hwui/gl/angle_gl.cc").read_text(encoding="utf-8")
    start = "bool gesher_hwui_gl_usable(std::string* detail, bool require_hardware) {"
    end = "// --- the debug read-back"
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("Production probe extraction boundaries changed")
    probe = source[source.index(start):source.index(end)]
    if probe.count("GrDirectContexts::MakeGL(") != 1:
        raise ValueError("Ganesh creation is no longer qualified exactly once")
    fixture = (ROOT / "tests/hwui-renderer/ganesh_probe_test.cc.in").read_text(encoding="utf-8")
    if fixture.count("@PROBE@") != 1:
        raise ValueError("Test fixture insertion marker missing")
    with tempfile.TemporaryDirectory(prefix="gesher-ganesh-probe-") as temp:
        code = Path(temp) / "ganesh_probe_test.cc"
        binary = Path(temp) / ("ganesh_probe_test.exe" if os.name == "nt" else "ganesh_probe_test")
        code.write_text(fixture.replace("@PROBE@", probe), encoding="utf-8")
        cmd = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-I", str(ROOT / "tests/hwui-renderer/stubs"), "-I", str(ROOT),
            str(code), str(ROOT / "compat/hwui/gl/pipeline_select.cc"), "-o", str(binary)]
        if args.sanitize:
            cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer"]
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
