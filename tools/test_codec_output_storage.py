#!/usr/bin/env python3
"""Exercise the production MediaCodec output representation with host boundaries.

The exact Slot/copy, EmitFrame, getters, Publish and FreeOutput implementations
are compiled against controlled JNI/window/decoder boundaries. Pixel/crop,
metadata, ownership, backpressure and output-mode transitions are checked. The
benchmark counts memcpy payload and retained output-slot capacity; it does not
measure the decoder, Windows/ART, GPU transport or whole-process memory.

--compare-ref runs the same fixture on a baseline and candidate and requires
identical CPU-output work plus zero duplicate Surface-frame bytes in candidate.
--source-ref with no --baseline runs today's savings assertions on old code,
providing an intentionally failing negative control.
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
SOURCE = "compat/media/jni/media_codec_jni.cc"


def source_text(ref: str | None) -> str:
    if ref:
        return subprocess.run(["git", "show", ref + ":" + SOURCE], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout
    return (ROOT / SOURCE).read_text(encoding="utf-8")


def generate(source: str) -> str:
    start, end = "struct Slot {", "class Codec {"
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("production Slot/copy boundaries changed")
    replacements = {"SLOT_AND_COPY": source[source.index(start):source.index(end)]}
    for name, marker in {
        "SET_SURFACE": "  void SetSurfaceLocked(",
        "HAS_SURFACE": "  bool has_surface() const",
        "INITIALIZED": "  bool CheckInitializedLocked(",
        "OUTPUT_BUFFER": "  jobject OutputBuffer(",
        "OUTPUT_IMAGE": "  jobject OutputImage(",
        "BUFFERS": "  jobjectArray Buffers(",
        "FREE_OUTPUT": "  int FreeOutput(",
        "PUBLISH": "  void Publish(",
        "EMIT_FRAME": "  bool EmitFrame(",
        "EMIT_EOS": "  void EmitEos(",
    }.items():
        replacements[name] = method(source, marker)
    template = (ROOT / "tests/media/codec_output_storage_host.cc.in").read_text()
    for name, body in replacements.items():
        marker = "@" + name + "@"
        if template.count(marker) != 1:
            raise ValueError("fixture boundary changed: " + marker)
        template = template.replace(marker, body)
    return ("#define HAS_DEFERRED_OUTPUT " + str(int("uint8_t* CpuOutputBytes(" in source))
            + "\n" + template)


def run(work: Path, label: str, source: str, args, baseline: bool = False) -> dict:
    cpp = work / (label + ".cc")
    cpp.write_text(generate(source))
    binary = work / label
    command = shlex.split(args.cxx) + [
        "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", "-fno-exceptions",
        "-I", str(ROOT), str(cpp), "-o", str(binary)]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True, timeout=120)
    result = subprocess.run([str(binary)] + (["--baseline"] if baseline else []),
                            capture_output=True, text=True, timeout=90)
    print(label + " production SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
    rows = [json.loads(line.removeprefix("MEASURE "))
            for line in result.stdout.splitlines() if line.startswith("MEASURE ")]
    if len(rows) != 9:
        raise RuntimeError("incomplete measurement output")
    return {"source_sha256": hashlib.sha256(source.encode()).hexdigest(), "measurements": rows}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--compare-ref")
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.compare_ref and (args.source_ref or args.baseline):
        parser.error("--compare-ref cannot be combined with --source-ref or --baseline")
    report = {"scope": "exact production output methods, controlled host JNI/window/decoder boundaries",
              "sanitized": args.sanitize}
    with tempfile.TemporaryDirectory(prefix="gesher-codec-output-") as temporary:
        work = Path(temporary)
        if args.compare_ref:
            report["baseline_ref"] = args.compare_ref
            report["baseline"] = run(work, "baseline", source_text(args.compare_ref), args, True)
        report["candidate"] = run(work, "candidate", source_text(args.source_ref), args, args.baseline)
    if "baseline" in report:
        for before, after in zip(report["baseline"]["measurements"], report["candidate"]["measurements"]):
            for key in ("mode", "width", "height", "frames", "frame_bytes"):
                if before[key] != after[key]:
                    raise AssertionError("baseline/candidate workload differs: " + key)
            if after["mode"] == "cpu":
                for key in ("copied_bytes", "slot_capacity_bytes"):
                    if before[key] != after[key]:
                        raise AssertionError("CPU representation changed: " + key)
            elif not (before["copied_bytes"] > 0 and before["slot_capacity_bytes"] > 0
                      and after["copied_bytes"] == 0 and after["slot_capacity_bytes"] == 0):
                raise AssertionError("Surface duplicate-storage reduction missing")
        print("A/B: identical CPU output; Surface duplicate frame copies/capacity eliminated")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
