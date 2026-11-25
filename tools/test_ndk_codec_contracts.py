#!/usr/bin/env python3
"""Complete codec/NDK units with explicit host codec/metadata/window boundaries.

Checks registration, LP64 indexes, callback reentrancy, format ownership and
persistent input leases; no Windows or actual media decoding is claimed. The
separate NDK ELF/JNI fixture runs on Windows with real backends.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/media/ndk-codec-host"

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    paths = ["codec/codec_engine.cc", "codec/codec_engine.h", "codec/callback_queue.h",
             "ndk/media_codec.cc", "ndk/media_codec_abi.h", "encode/encoder.h",
             "encode/codec_config.cc", "encode/codec_config.h", "video/video_decoder.h",
             "video/yuv_rgba.h", "player/extractor.h", "player/data_source.h",
             "player/decoder.h", "audio/engine.h", "audio/log.h"]
    with tempfile.TemporaryDirectory(prefix="gesher-ndk-codec-") as temporary:
        work = Path(temporary)
        for relative in paths:
            source = ROOT / "compat/media" / relative
            target = work / "compat/media" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            data = source.read_bytes()
            target.write_bytes(data)
            print(relative, hashlib.sha256(data).hexdigest(), flush=True)
        surface = work / "compat/media/surface/queue_surface.h"
        surface.parent.mkdir(parents=True)
        shutil.copyfile(FIXTURE / "queue_surface.h", surface)
        binary = work / "ndk-codec-host"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-pthread", "-Wall",
            "-Wextra", "-Werror", "-Wno-unused-parameter", "-DGESHER_MEDIA_STANDALONE",
            "-I", str(work), "-I", str(FIXTURE / "include"),
            "-I", str(ROOT / "compat/bionic/include"), str(FIXTURE / "contracts.cc"),
            *[str(work / "compat/media" / p) for p in paths if p.endswith(".cc")],
            "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=45).returncode

if __name__ == "__main__":
    raise SystemExit(main())
