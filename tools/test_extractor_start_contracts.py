#!/usr/bin/env python3
"""Exercise actual extractor startup, read errors and cleanup on a host.

The plugin's start/stop/free, buffer group, AMediaFormat storage and error
translator are declared seams. No container plugin, Windows or ART runs here.
The complete NDK adapter and Android error translators are checked separately.
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


def body(source: str, marker: str) -> str:
    if source.count(marker) != 1:
        raise ValueError("production boundary changed: " + marker)
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise ValueError("unbalanced production boundary: " + marker)
    return source[start:end]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--out", type=Path, help="Retain generated source, logs and executable")
    args = parser.parse_args()
    temporary = None if args.out else tempfile.TemporaryDirectory(prefix="gesher-extractor-start-")
    work = args.out.resolve() if args.out else Path(temporary.name)
    work.mkdir(parents=True, exist_ok=True)
    try:
        header_dir = work / "include/media"
        header_dir.mkdir(parents=True, exist_ok=True)
        (header_dir / "NdkMediaFormat.h").write_bytes(
            (ROOT / "tests/media/ndk-extractor/host_format.h").read_bytes())
        extractor = (ROOT / "compat/media/player/extractor.cc").read_text()
        ndk = (ROOT / "compat/media/ndk/media_extractor.cc").read_text()
        first = extractor.index("Format::Format()")
        replacements = {
            "@EXTRACTOR_HEADER@": str(ROOT / "compat/media/player/extractor.h"),
            "@FORMAT_METHODS@": extractor[first:extractor.index("\nnamespace {", first)],
            "@TRACK_READER@": body(extractor, "class AospTrackReader : public TrackReader") + ";",
            "@SELECT@": body(ndk, "media_status_t Select("),
            "@CURRENT@": body(ndk, "Track* Current("),
            "@GET_SAMPLE_FORMAT@": body(ndk, "media_status_t GetSampleFormat("),
        }
        source = (ROOT / "tests/media/ndk-extractor/start_status_host.cc.in").read_text()
        for marker, value in replacements.items():
            if source.count(marker) != 1:
                raise ValueError("fixture boundary changed: " + marker)
            source = source.replace(marker, value)
        for name, value in [("reader", replacements["@TRACK_READER@"]),
                            ("selection", replacements["@SELECT@"])]:
            print(name + " SHA256 " + hashlib.sha256(value.encode()).hexdigest(), flush=True)
        (work / "probe.cc").write_text(source)
        command = shlex.split(args.cxx) + [
            "-std=c++23", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I" + str(work / "include"), str(work / "probe.cc"), "-o", str(work / "probe")]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        built = subprocess.run(command, capture_output=True, text=True, timeout=90)
        (work / "build.log").write_text(built.stdout + built.stderr)
        if built.returncode:
            print(built.stdout + built.stderr)
            return built.returncode
        ran = subprocess.run([str(work / "probe")], capture_output=True, text=True, timeout=20)
        (work / "run.log").write_text(ran.stdout + ran.stderr)
        print(ran.stdout + ran.stderr)
        return ran.returncode
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
