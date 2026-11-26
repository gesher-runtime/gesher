#!/usr/bin/env python3
"""Compose complete Recorder/InputSurface/MP4 with declared host encoder/queue APIs.

The encoder supplies an explicit pre-encoded AVC packet, not a Windows codec.
Actual source conversion, admission, worker, lease, muxer and failure cleanup run.
FFmpeg independently demuxes/decodes both real production MP4 outputs.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from test_codec_lifecycle_contracts import method

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    backend = (ROOT / "tests/media/recorder-host/lifecycle.cc").read_text().split("struct Events {")[0]
    for marker in ("InputSurface* CreateInputSurface(", "void VideoFrame::Allocate(", "void RgbaToI420("):
        backend = backend.replace(method(backend, marker), "")
    observer = ("static std::atomic<int> persistent_encoded{0};\n"
                "static std::mutex persistent_encoder_mu;\n"
                "static std::condition_variable persistent_encoder_cv;\n")
    backend = backend.replace("class HostVideoEncoder", observer + "class HostVideoEncoder", 1)
    boundary = "out->push_back({avc_access_unit, frame.time_us, true, false});"
    assert backend.count(boundary) == 1
    backend = backend.replace(boundary, boundary + "\n"
        "    { std::lock_guard<std::mutex> lock(persistent_encoder_mu); ++persistent_encoded; }\n"
        "    persistent_encoder_cv.notify_all();")
    video = (ROOT / "compat/media/video/video_decoder.cc").read_text()
    fixture = (ROOT / "tests/media/recorder-host/persistent.cc.in").read_text()
    for key, marker in (("ALLOCATE", "void VideoFrame::Allocate("), ("CLAMP", "inline uint8_t Clamp8("),
                        ("CONVERT", "void RgbaToI420(")):
        assert fixture.count("@" + key + "@") == 1
        fixture = fixture.replace("@" + key + "@", method(video, marker))
    with tempfile.TemporaryDirectory(prefix="gesher-recorder-persistent-") as directory:
        work = Path(directory)
        core = work / "compat/media/video/input_surface.cc"
        core.parent.mkdir(parents=True)
        for name in ("input_surface.cc", "input_surface.h"):
            (core.parent / name).write_bytes((ROOT / "compat/media/video" / name).read_bytes())
        queue = work / "compat/media/surface/queue_surface.h"
        queue.parent.mkdir(parents=True)
        queue.write_bytes((ROOT / "tests/media/input_surface_queue_host.h").read_bytes())
        runtime = work / "android_runtime/android_view_Surface.h"
        runtime.parent.mkdir()
        runtime.write_text('#pragma once\n#include <jni.h>\n'
                           '#include "compat/media/surface/queue_surface.h"\n'
                           'namespace android { jobject android_view_Surface_createFromSurface('
                           'JNIEnv*, const sp<Surface>&); }\n')
        unit = work / "fixture.cc"; unit.write_text(backend + fixture)
        media = ROOT / "compat/media"
        paths = [core, media / "record/recorder.cc", media / "mux/mp4_muxer.cc",
                 media / "mux/ogg_muxer.cc", media / "encode/codec_config.cc"]
        for path in paths:
            print(str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path.name) +
                  " SHA256 " + hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
        binary = work / "persistent-recorder"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra",
            "-Werror", "-Wno-unused-parameter", "-DGESHER_MEDIA_STANDALONE", "-I" + str(work),
            "-I" + str(ROOT), "-I" + str(ROOT / "tests/media/recorder-host/include"),
            str(unit), *map(str, paths), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        avc = work / "frame.h264"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=red:size=32x32",
                        "-frames:v", "1", "-c:v", "libx264", "-f", "h264", str(avc)], check=True, timeout=30)
        subprocess.run([str(binary), str(avc), str(work)], check=True, timeout=30)
        for session in range(2):
            path = work / f"persistent-{session}.mp4"
            result = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-count_packets",
                "-show_streams", "-of", "json", str(path)], text=True, timeout=30))
            assert len(result["streams"]) == 1
            stream = result["streams"][0]
            assert (stream["codec_name"], stream["width"], stream["height"], int(stream["nb_read_packets"])) == ("h264", 32, 32, 3)
            subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", str(path), "-f", "null", "-"], check=True, timeout=30)
        print("Persistent recorder MP4 outputs independently demuxed and decoded: two sessions, three frames each")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
