#!/usr/bin/env python3
"""Exact production timestamp and codec owner methods with declared host boundaries.

The real QueueSurface/ANativeWindow fixture is separate and runs on Windows.
This does not establish complete MediaCodec-object lifetime under MC_release.
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
        raise ValueError("production boundaries changed: " + start)
    return source[source.index(start):source.index(end, source.index(start))]


def source_text(relative: str, ref: str | None) -> str:
    if ref:
        return subprocess.run(["git", "show", ref + ":" + relative], cwd=ROOT,
                              capture_output=True, check=True, text=True).stdout
    return (ROOT / relative).read_text(encoding="utf-8")


def generate(component: str, ref: str | None) -> str:
    if component == "codec":
        source = source_text("compat/media/jni/media_codec_jni.cc", ref)
        template = (ROOT / "tests/media/codec_window_host.cc.in").read_text(encoding="utf-8")
        replacements = {
            "@SET_SURFACE@": section(source, "  void SetSurface(JNIEnv* env, jobject surface)",
                                      "  bool has_surface() const"),
            "@RELEASE_OUTPUT@": section(source, "  bool ReleaseOutput(int index, bool render,",
                                        "  jobject InputBuffer(JNIEnv* env, int index)")}
    else:
        source = source_text("compat/media/surface/queue_surface.cc", ref)
        header = source_text("compat/media/surface/queue_surface.h", ref)
        members = re.findall(r"  int64_t timestamp_ns_ = [^;]+;", header)
        if len(members) != 1:
            raise ValueError("timestamp member boundary changed")
        template = (ROOT / "tests/media/surface_timestamp_host.cc.in").read_text(encoding="utf-8")
        replacements = {
            "@TIMESTAMP_MEMBER@": members[0].strip(),
            "@TIMESTAMP_DISPATCH@": section(source, "    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP: {",
                                            "    case NATIVE_WINDOW_GET_REFRESH_CYCLE_DURATION:"),
            "@PUBLISH@": section(source, "void QueueSurface::Publish() {",
                                  "int QueueSurface::dequeueBuffer(")}
        source += header
    for marker, body in replacements.items():
        if template.count(marker) != 1:
            raise ValueError("template marker changed: " + marker)
        template = template.replace(marker, body)
    digest = hashlib.sha256(source.encode()).hexdigest()
    print(component + " source SHA256 " + digest, flush=True)
    return "// Production source SHA256: " + digest + "\n" + template


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--generate-codec", type=Path, help="Emit the portable exact-method fixture for CMake")
    args = parser.parse_args()
    if args.generate_codec:
        args.generate_codec.write_text(generate("codec", args.source_ref), encoding="utf-8")
        return 0
    with tempfile.TemporaryDirectory(prefix="gesher-media-surface-") as temporary:
        work = Path(temporary)
        failed = False
        for component in ["surface", "codec"]:
            source = work / (component + ".cc")
            source.write_text(generate(component, args.source_ref), encoding="utf-8")
            binary = work / component
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(source), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            failed |= subprocess.run([str(binary)], check=False, timeout=30).returncode != 0
        return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
