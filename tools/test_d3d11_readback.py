#!/usr/bin/env python3
"""Exercise production D3D11 readback/conversion methods at an explicit D3D boundary.

The device, texture, context and Media Foundation objects are host adapters.
The staging allocator, all three extraction methods, Decode, VideoFrame::Allocate
and Nv12ToI420 are exact source.
This checks pixels, pitch, ownership, errors and CPU allocation/copy counts; it
does not measure real GPU readback, Media Foundation or Windows/ART playback.
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


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("production extraction boundary changed: " + start)
    return source[source.index(start):source.index(end, source.index(start))]


def read(path: str, ref: str | None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + path], cwd=ROOT, text=True)
    return (ROOT / path).read_text(encoding="utf-8")


def generate(ref: str | None) -> str:
    decoder = read("compat/media/video/d3d11_decoder.cc", ref)
    video = read("compat/media/video/video_decoder.cc", ref)
    header = read("compat/media/video/video_decoder.h", ref)
    start = "struct D3D11VideoDecoder::Nv12Readback {"
    if start not in decoder:
        start = "bool D3D11VideoDecoder::ExtractNv12("
    bodies = {
        "@FRAME@": section(header, "struct VideoFrame {", "// The codecs (MediaFormat"),
        "@ALLOCATE@": section(video, "void VideoFrame::Allocate(",
                               "const std::vector<VideoCodecInfo>& VideoDecoder::Available()"),
        "@CONVERT@": section(video, "void Nv12ToI420(", "}  // namespace gesher::media"),
        "@STAGING@": section(decoder, "bool D3D11VideoDecoder::EnsureStagingTexture(",
                              "bool D3D11VideoDecoder::InitDecoder("),
        "@EXTRACT@": section(decoder, start, "bool D3D11VideoDecoder::DeliverToImageReader("),
        "@DECODE@": section(decoder, "bool D3D11VideoDecoder::Decode(", "void D3D11VideoDecoder::Flush()"),
    }
    source = (ROOT / "tests/media/d3d11_readback_host.cc.in").read_text(encoding="utf-8")
    for marker, body in bodies.items():
        if source.count(marker) != 1:
            raise ValueError("fixture marker changed: " + marker)
        source = source.replace(marker, body)
    digest = hashlib.sha256((decoder + video + header).encode()).hexdigest()
    print("D3D11 readback production SHA256 " + digest, flush=True)
    return "// Production source SHA256: " + digest + "\n" + source


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--compatibility-only", action="store_true",
                        help="Only valid layouts and existing error contracts (safe baseline control)")
    parser.add_argument("--measure", action="store_true",
                        help="Print counts without enforcing the improved allocation/copy bounds")
    parser.add_argument("--benchmark", action="store_true", help="Include informational host CPU timings")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--generate", type=Path, help="Write the exact-method fixture without compiling")
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")
    source = generate(args.source_ref)
    if args.generate:
        args.generate.parent.mkdir(parents=True, exist_ok=True)
        args.generate.write_text(source, encoding="utf-8")
        return 0
    with tempfile.TemporaryDirectory(prefix="gesher-d3d11-readback-") as temporary:
        work = Path(temporary)
        cpp = work / "readback.cc"
        cpp.write_text(source, encoding="utf-8")
        binary = args.output.resolve() if args.output else work / ("readback.exe" if os.name == "nt" else "readback")
        binary.parent.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        if args.compile_only:
            print("Compiled D3D11 host-boundary fixture: " + str(binary), flush=True)
            return 0
        flags = [flag for enabled, flag in [(args.compatibility_only, "--compatibility-only"),
                 (args.measure, "--measure"), (args.benchmark, "--benchmark")] if enabled]
        return subprocess.run([str(binary), *flags], check=False, timeout=90).returncode


if __name__ == "__main__":
    raise SystemExit(main())
