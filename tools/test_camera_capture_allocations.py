#!/usr/bin/env python3
"""Exercise production camera nativeRead with a frame-producing capture/JNI boundary.

The real Capture state and trace code are included. Fixtures cover native scratch
allocation, publication on failure, concurrent reads and destruction. No Media
Foundation or physical-camera latency claim is made.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("camera capture extraction boundary changed")
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--measure-only", action="store_true")
    parser.add_argument("--valid-only", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    def read(path: str) -> str:
        return ((ROOT / path).read_text() if not args.source_ref else
                subprocess.run(["git", "show", args.source_ref + ":" + path], cwd=ROOT,
                               capture_output=True, text=True, check=True).stdout)

    trace = re.sub(r"^\s*#(?:include|pragma).*\n", "", read("compat/devices/camera_trace.h"), flags=re.MULTILINE)
    header = read("compat/devices/camera_mf.h")
    # Keep Capture's real typed state with its declaration. Historical sources
    # predate backend provenance and still start at the class itself.
    capture = section(header, "enum class CaptureBackend" if "enum class CaptureBackend" in header
                      else "class Capture {", "// Opens `id`")
    native = section(read("compat/devices/devices_jni.cc"), "jint WindowsCamera_read(", "void WindowsCamera_close(")
    fixture = (ROOT / "tests/media/camera_capture_allocations_host.cc.in").read_text()
    fixture = fixture.replace("@TRACE@", trace)
    fixture = fixture.replace("@CAPTURE@", capture).replace("@JNI@", native)
    print("camera capture source SHA256 " + hashlib.sha256((trace + capture + native).encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-camera-capture-") as temporary:
        work = Path(temporary)
        cpp = work / "capture.cc"
        cpp.write_text(fixture)
        binary = work / ("capture.exe" if os.name == "nt" else "capture")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra",
                                         "-Werror", "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        runtime_env = os.environ.copy()
        compiler = Path(command[0])
        if os.name == "nt" and compiler.is_absolute():
            runtime_env["PATH"] = str(compiler.parent) + os.pathsep + runtime_env.get("PATH", "")
        subprocess.run(command, check=True, timeout=120)
        mode = "measure" if args.measure_only else "valid-gate" if args.valid_only else "gate"
        result = subprocess.run([str(binary), mode], check=False, timeout=120, env=runtime_env)
        if result.returncode:
            print(f"Camera capture fixture failed with exit code {result.returncode}", flush=True)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
