#!/usr/bin/env python3
"""Compile exact codec input admission, lease setters and I420 conversion bodies.

Production InputSurface is complete, with the explicit host Queue/JNI adapter.
The worker, codec object fields and conversion barrier are declared boundaries.
--source-ref measures the old actual OnSurfaceFrame with the same data/counters;
it does not invent the missing persistent APIs on the baseline.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_codec_lifecycle_contracts import method

ROOT = Path(__file__).resolve().parents[1]


def read(path: str, ref: str | None = None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + path], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    codec = read("compat/media/jni/media_codec_jni.cc", args.source_ref)
    video = read("compat/media/video/video_decoder.cc", args.source_ref)
    persistent = "  bool SetInputSurface(" in codec
    replacements = {
        "ALLOCATE": method(video, "void VideoFrame::Allocate("),
        "CLAMP": method(video, "inline uint8_t Clamp8("),
        "CONVERT": method(video, "void RgbaToI420("),
        "CREATE_INPUT": method(codec, "  jobject CreateInputSurface("),
        "SET_INPUT": method(codec, "  bool SetInputSurface(") if persistent else "",
        "ON_FRAME": method(codec, "  void OnSurfaceFrame("),
    }
    template = read("tests/media/codec_input_host.cc.in")
    for key, body in replacements.items():
        if template.count("@" + key + "@") != 1:
            raise ValueError("fixture boundary changed: " + key)
        template = template.replace("@" + key + "@", body)
    print("codec input source SHA256 " + hashlib.sha256(codec.encode()).hexdigest(), flush=True)
    print("pixel source SHA256 " + hashlib.sha256(video.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-codec-input-") as directory:
        work = Path(directory)
        for name in ("input_surface.cc", "input_surface.h"):
            path = "compat/media/video/" + name
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(read(path))
        queue = work / "compat/media/surface/queue_surface.h"
        queue.parent.mkdir(parents=True)
        queue.write_text(read("tests/media/input_surface_queue_host.h"))
        (work / "jni.h").write_text("#pragma once\nstruct JNIEnv;\nusing jobject = void*;\n")
        runtime = work / "android_runtime/android_view_Surface.h"
        runtime.parent.mkdir()
        runtime.write_text('#pragma once\n#include "jni.h"\n'
                           '#include "compat/media/surface/queue_surface.h"\n'
                           'namespace android { jobject android_view_Surface_createFromSurface('
                           'JNIEnv*, const sp<Surface>&); }\n')
        source = work / "codec-input.cc"
        source.write_text(f"#define HAS_PERSISTENT_INPUT {int(persistent)}\n" + template)
        binary = work / "codec-input"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-pthread", "-Wall", "-Wextra",
            "-Werror", "-Wno-mismatched-new-delete", "-I" + str(work), "-I" + str(ROOT),
            "-I" + str(ROOT / "tests/media/recorder-host/include"), str(source),
            str(work / "compat/media/video/input_surface.cc"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=90).returncode


if __name__ == "__main__":
    raise SystemExit(main())
