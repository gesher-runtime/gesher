#!/usr/bin/env python3
"""Exercise the production retriever PTS/EOF selection and metadata adapter.

Exact current function/class bodies are copied into a controlled extractor and
decoder boundary fixture. This is not a Windows Media Foundation or JNI/Bitmap
test; the existing videoapp blue/white/red pixel contract provides that gate.
Old JNI/reader sources can be selected for a same-fixture negative control.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("production source boundaries changed")
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jni-source", type=Path,
                        default=ROOT / "compat/media/jni/media_jni_others.cc")
    parser.add_argument("--reader-source", type=Path,
                        default=ROOT / "compat/media/player/extractor.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    jni = args.jni_source.read_text(encoding="utf-8")
    reader = args.reader_source.read_text(encoding="utf-8")
    start = "std::unique_ptr<VideoFrame> DecodeRetrieverFrame("
    if start in jni:
        selector = section(jni, start, "// _getFrameAtTime(timeUs,")
    else:
        # The previous implementation was an inline portion of the JNI method.
        body = section(jni, "  // OPTION_PREVIOUS_SYNC 0,", "  if (chosen == nullptr) return nullptr;")
        selector = ("std::unique_ptr<VideoFrame> DecodeRetrieverFrame(TrackReader* reader, "
                    "VideoDecoder* decoder, int64_t time_us, int option) {\n"
                    + body + "  return chosen;\n}\n")
    adapter = section(reader, "class AospTrackReader :", "class AospExtractor :")
    template = (ROOT / "tests/media/retriever_frame_host.cc.in").read_text(encoding="utf-8")
    for marker, body in (("@TRACK_READER@", adapter), ("@RETRIEVER_SELECTION@", selector)):
        if template.count(marker) != 1:
            raise ValueError("host fixture marker changed: " + marker)
        template = template.replace(marker, body)
    for name, source in (("retriever", jni), ("reader", reader)):
        print(name + " source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-retriever-") as tmp:
        folder = Path(tmp)
        cpp = folder / "retriever.cc"
        cpp.write_text(template, encoding="utf-8")
        binary = folder / "retriever"
        command = shlex.split(args.cxx) + [
            "-std=c++17", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
            str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
