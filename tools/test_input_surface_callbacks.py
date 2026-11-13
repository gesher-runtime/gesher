#!/usr/bin/env python3
"""Compile complete production InputSurface with declared queue/JNI boundaries.

The same assertions execute complete QueueSurface/hostgraphics on Windows.
The host-only stale-listener case models a listener copied before abandonment.
Lifetime is checked through ordinary admitted calls, not invalid memory access.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def production(path: str, ref: str | None) -> bytes:
    if ref:
        return subprocess.run(["git", "show", ref + ":" + path], cwd=ROOT,
                              capture_output=True, check=True).stdout
    return (ROOT / path).read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile the original production implementation")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-input-surface-") as temporary:
        work = Path(temporary)
        for name in ("input_surface.cc", "input_surface.h"):
            path = "compat/media/video/" + name
            source = production(path, args.source_ref)
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source)
            print(path + " SHA256 " + hashlib.sha256(source).hexdigest(), flush=True)
        queue = work / "compat/media/surface/queue_surface.h"
        queue.parent.mkdir(parents=True)
        queue.write_bytes((ROOT / "tests/media/input_surface_queue_host.h").read_bytes())
        (work / "jni.h").write_text("#pragma once\nstruct JNIEnv;\nusing jobject = void*;\n")
        runtime = work / "android_runtime/android_view_Surface.h"
        runtime.parent.mkdir()
        runtime.write_text(
            '#pragma once\n#include "jni.h"\n'
            '#include "compat/media/surface/queue_surface.h"\n'
            'namespace android { jobject android_view_Surface_createFromSurface('
            'JNIEnv*, const sp<Surface>&); }\n')
        binary = work / "callbacks"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-DGESHER_INPUT_SURFACE_HOST", "-I" + str(work),
            str(work / "compat/media/video/input_surface.cc"),
            str(ROOT / "tests/media/input_surface_callback_test.cc"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
