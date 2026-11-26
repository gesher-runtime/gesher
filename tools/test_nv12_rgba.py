#!/usr/bin/env python3
"""Exercise actual NV12 delivery, exhaustive colors, strides and reader ownership.

Production converters, CameraFrames_deliver, QueueFrame, EnqueueImage, acquire,
release and close are extracted without modification. JNI and ANativeWindow
adapters are explicit isolated fixtures, not ART, a GPU, or a physical camera.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

import test_codec_reader_delivery as shared

ROOT = Path(__file__).resolve().parents[1]


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError("Ambiguous fixture boundary: " + before)
    return source.replace(before, after, 1)


def generate(ref=None):
    source, hashes = shared.generate(ref)
    start = source.index("struct ANativeWindow {};")
    end = source.index("namespace gesher::media { struct ReaderSurface", start)
    source = source[:start] + (ROOT / "tests/media/nv12_rgba_window.inc").read_text() + source[end:]
    source = replace_once(source, "int main(int argc, char**)",
                          "int LegacyCodecReaderMain(int argc, char**)")
    source = replace_once(source, "[[gnu::noinline]] void* operator new(size_t size)",
                          "#ifndef NV12_RGBA_TIMING\n[[gnu::noinline]] void* operator new(size_t size)")
    source = replace_once(source, "struct Uncounted {", "#endif\nstruct Uncounted {")
    fixture = (ROOT / "tests/media/nv12_rgba_contract.inc").read_text()
    for path in ["tests/media/codec_reader_delivery_host.cc.in",
                 "tests/media/nv12_rgba_window.inc", "tests/media/nv12_rgba_contract.inc",
                 "tools/test_codec_reader_delivery.py"]:
        hashes[path] = hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
    return source + "\n" + fixture, hashes


def compile_source(work, label, source, args):
    cpp = work / (label + ".cc")
    binary = work / (label + (".exe" if os.name == "nt" else ""))
    cpp.write_text(source)
    command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
               "-fno-exceptions", "-pthread", "-I", str(ROOT), str(cpp), "-o", str(binary)]
    if args.timing:
        command += ["-DNV12_RGBA_TIMING"]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True, timeout=120)
    return binary, command


def negative_controls(work, source, args):
    controls = [
        ("wrong_chroma", "const int red = 409 * e + 128;", "const int red = 409 * d + 128;",
         "NV12 complete RGBA pixel oracle"),
        ("wrong_luma_row", "const uint8_t* y1 = y0 + w;", "const uint8_t* y1 = y0;",
         "NV12 complete RGBA pixel oracle"),
        ("wrong_stride", "uint8_t* row1 = row0 + static_cast<size_t>(stride) * 4;",
         "uint8_t* row1 = row0 + static_cast<size_t>(w) * 4;", "NV12 padded window pixel and guard oracle"),
        ("wrong_rounding", "const int blue = 516 * d + 128;", "const int blue = 516 * d;",
         "NV12 complete RGBA pixel oracle"),
        ("close_publication", "if (r->closed) { delete image; return NO_INIT; }",
         "if (false && r->closed) { delete image; return NO_INIT; }",
         "NV12 close racing conversion cannot publish"),
    ]
    receipts = []
    for name, before, after, expected in controls:
        changed = replace_once(source, before, after)
        mutant, command = compile_source(work, name, changed, args)
        result = subprocess.run([str(mutant), "--quick"], text=True, capture_output=True, timeout=120)
        (work / (name + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode != 1 or "FAIL reader delivery: " + expected not in result.stderr:
            raise AssertionError("Mutant missed intended assertion: " + name + "\n" + result.stderr)
        print("NV12_RGBA_NEGATIVE_PASS", name, expected, flush=True)
        receipts.append({"name": name, "assertion": expected, "command": command,
            "returncode": result.returncode, "source_sha256": hashlib.sha256(changed.encode()).hexdigest()})
    return receipts


def run(work, args):
    source, hashes = generate(args.source_ref)
    binary, command = compile_source(work, "nv12-rgba", source, args)
    metadata = {"source_ref": args.source_ref, "source_sha256": hashes,
                "generated_sha256": hashlib.sha256(source.encode()).hexdigest(),
                "executable_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "command": command, "timing": args.timing, "sanitized": args.sanitize,
                "compiler": subprocess.check_output(shlex.split(args.cxx) + ["--version"], text=True)}
    (work / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if not args.build_only:
        result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=120)
        (work / "contracts.log").write_text(result.stdout + result.stderr)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            raise AssertionError(result.stderr or "NV12 RGBA contracts failed")
    if args.negative_controls:
        metadata["negative_controls"] = negative_controls(work, source, args)
        (work / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("NV12_RGBA_SOURCES", json.dumps(hashes, sort_keys=True), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--timing", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--build-output", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    if args.timing and (args.sanitize or args.negative_controls):
        parser.error("ordinary timing must exclude sanitizers and observers")
    if args.negative_controls and args.source_ref:
        parser.error("negative controls require the current candidate")
    if args.build_output:
        args.build_output.mkdir(parents=True, exist_ok=True)
        if any(args.build_output.iterdir()):
            parser.error("build output must be empty")
        run(args.build_output, args)
    else:
        with tempfile.TemporaryDirectory(prefix="gesher-nv12-rgba-") as temporary:
            run(Path(temporary), args)


if __name__ == "__main__":
    main()
