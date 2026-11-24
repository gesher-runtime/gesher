#!/usr/bin/env python3
"""Run the complete production MF video decoder at a declared Win32/MF host boundary.

The adapter supplies owned, reference-counted MF buffers (1D and 2D) and samples.
The real MF decoder, Annex B parameter-set preparation, NV12-to-I420 conversion,
stream-change renegotiation and teardown execute unchanged. This does not execute
Windows Media Foundation or a hardware/software codec MFT.
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


def load_source(relative: str, source_ref: str | None) -> bytes:
    if not source_ref:
        return (ROOT / relative).read_bytes()
    return subprocess.check_output(["git", "show", source_ref + ":" + relative], cwd=ROOT)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--measure-only", action="store_true",
                        help="Run valid-input compatibility and report measurements without budgets")
    parser.add_argument("--valid-only", action="store_true",
                        help="Run valid compatibility checks and skip failure/reuse gates unsafe on baseline")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    mf_video_rel = "compat/media/video/mf_video.cc"
    video_dec_rel = "compat/media/video/video_decoder.cc"
    mf_video_src = load_source(mf_video_rel, args.source_ref)
    video_dec_src = load_source(video_dec_rel, args.source_ref)
    print("MF video source SHA256 " + hashlib.sha256(mf_video_src).hexdigest(), flush=True)
    print("Video decoder source SHA256 " + hashlib.sha256(video_dec_src).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-mf-video-") as temporary:
        work = Path(temporary)
        mf_video_tu = work / "mf_video.cc"
        mf_video_tu.write_bytes(mf_video_src)
        video_dec_tu = work / "video_decoder.cc"
        video_dec_tu.write_bytes(video_dec_src)
        for name in ("windows.h", "mfapi.h", "mferror.h", "mfidl.h", "mftransform.h", "objbase.h"):
            (work / name).write_text('#include "tests/media/mf_video_host.h"\n')
        ndk_dir = work / "media"
        ndk_dir.mkdir(parents=True, exist_ok=True)
        (ndk_dir / "NdkMediaFormat.h").write_text('#include "tests/media/mf_video_host.h"\n')
        binary = work / "mf_video_output"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-D_GLIBCXX_ASSERTIONS",
            "-DGESHER_MEDIA_STANDALONE", '-DGESHER_MF_VIDEO_SOURCE="' + str(mf_video_tu) + '"',
            "-I", str(work), "-I", str(ROOT), "-I", str(ROOT / "compat/media/video"),
            str(ROOT / "tests/media/mf_video_output_host.cc"),
            str(video_dec_tu), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        mode = "measure" if args.measure_only else "valid-gate" if args.valid_only else "gate"
        return subprocess.run([str(binary), mode], check=False, timeout=120).returncode


if __name__ == "__main__":
    raise SystemExit(main())
