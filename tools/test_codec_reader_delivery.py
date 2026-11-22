#!/usr/bin/env python3
"""Check actual MediaCodec/ImageReader delivery against a pixel/lifetime oracle.

Production reader, queue, acquisition, close, converters and codec delivery are
compiled together. JNI/Java arrays and ANativeWindow are explicit host adapters.
A/B counts real native allocations and the minimum Java array/copy payload;
Java pinning intentionally makes no extra copy. It is not a Windows timing or
whole-process memory measurement.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_media_surface_contracts import method, section

ROOT = Path(__file__).resolve().parents[1]
READER = "compat/devices/camera_frames.cc"
CODEC = "compat/media/jni/media_codec_jni.cc"


def read_source(path: str, ref: str | None) -> str:
    if ref:
        return subprocess.run(["git", "show", f"{ref}:{path}"], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout
    return (ROOT / path).read_text()


def generate(ref: str | None) -> tuple[str, dict]:
    reader, codec = read_source(READER, ref), read_source(CODEC, ref)
    native = "int QueueVideoFrame(" in reader
    replacements = {
        "TYPES": section(reader, "struct Plane {", "struct {\n  JavaVM* vm;"),
        "LOOKUP": method(reader, "std::shared_ptr<Reader> ReaderForSurface("),
        "READER_OF": method(reader, "std::shared_ptr<Reader> ReaderOf("),
        "ENQUEUE": method(reader, "int EnqueueImage("),
        "RELEASE": method(reader, "void ReleaseImage("),
        "ACQUIRE": method(reader, "jint ImageReader_imageSetup("),
        "CLOSE": method(reader, "void ImageReader_close("),
        "NV12_CONVERT": section(reader, "void Nv12ToI420(", "// One frame into a reader:"),
        "QUEUE_FRAME": method(reader, "int QueueFrame("),
        "JNI_DELIVERY": method(reader, "jint CameraFrames_deliver("),
        "NATIVE_DELIVERY": (method(reader, "void CopyReaderPlane(") + "\n" +
                            method(reader, "int QueueVideoFrame(") if native else ""),
        "NATIVE_ENTRY": method(reader, "int QueueImageReaderVideoFrame(") if native else "",
        "CODEC_DELIVERY": method(codec, "int DeliverToReader("),
    }
    fixture = (ROOT / "tests/media/codec_reader_delivery_host.cc.in").read_text()
    for name, body in replacements.items():
        marker = "@" + name + "@"
        if fixture.count(marker) != 1:
            raise ValueError("fixture marker changed: " + marker)
        fixture = fixture.replace(marker, body)
    hashes = {p: hashlib.sha256(s.encode()).hexdigest() for p, s in [(READER, reader), (CODEC, codec)]}
    return f"#define NATIVE_DELIVERY {int(native)}\n" + fixture, hashes


def run(work: Path, label: str, ref: str | None, args, baseline: bool) -> dict:
    source, hashes = generate(ref)
    cpp = work / (label + ".cc")
    cpp.write_text(source)
    binary = work / (label + (".exe" if os.name == "nt" else ""))
    command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                    "-fno-exceptions", "-pthread", "-I", str(ROOT), str(cpp), "-o", str(binary)]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True, timeout=120)
    runtime_env = os.environ.copy()
    compiler = Path(command[0])
    if os.name == "nt" and compiler.is_absolute():
        runtime_env["PATH"] = str(compiler.parent) + os.pathsep + runtime_env.get("PATH", "")
    result = subprocess.run([str(binary)] + (["--baseline"] if baseline else []),
                            capture_output=True, text=True, timeout=120, env=runtime_env)
    print(label + " production hashes " + json.dumps(hashes), flush=True)
    print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    rows = [json.loads(line[8:]) for line in result.stdout.splitlines() if line.startswith("MEASURE ")]
    if len(rows) != 3:
        raise ValueError("missing measurements")
    return {"source_sha256": hashes, "measurements": rows}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare-ref")
    parser.add_argument("--source-ref")
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if args.compare_ref and (args.source_ref or args.baseline):
        parser.error("--compare-ref cannot be combined with --source-ref or --baseline")
    report = {"scope": __doc__.strip(), "sanitized": args.sanitize,
              "asan_options": os.environ.get("ASAN_OPTIONS", ""),
              "platform": platform.platform(), "compiler": args.cxx}
    with tempfile.TemporaryDirectory(prefix="gesher-codec-reader-") as temporary:
        work = Path(temporary)
        if args.compare_ref:
            report["baseline_ref"] = args.compare_ref
            report["baseline"] = run(work, "baseline", args.compare_ref, args, True)
        report["candidate"] = run(work, "candidate", args.source_ref, args, args.baseline)
    if "baseline" in report:
        for old, new in zip(report["baseline"]["measurements"], report["candidate"]["measurements"]):
            for field in ["width", "height", "frames", "checksum", "retained_image_bytes"]:
                if old[field] != new[field]:
                    raise AssertionError("output/work differs: " + field)
            payload = new["width"] * new["height"] * 3 // 2 * new["frames"]
            if (old["native_allocated_bytes"] - new["native_allocated_bytes"] != payload
                    or old["java_array_bytes"] != payload or new["java_array_bytes"] != 0
                    or old["java_copy_bytes"] != payload or new["java_copy_bytes"] != 0):
                raise AssertionError("full-frame intermediate reduction missing")
        print("A/B passed: identical images; one native and one Java frame allocation/copy eliminated per delivery")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
