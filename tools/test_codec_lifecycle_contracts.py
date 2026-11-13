#!/usr/bin/env python3
"""Exercise exact Codec receiver/lifecycle code with declared JNI/decoder seams.

The receiver fixture observes destruction without accessing a retired receiver.
The wait fixture controls the moment a signalled waiter reacquires its mutex;
it does not replace the production predicates, lifecycle or interruption code.
Actual public MediaCodec JNI calls execute separately in the Windows ART gate.
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
SOURCE = "compat/media/jni/media_codec_jni.cc"


def method(source: str, marker: str) -> str:
    if source.count(marker) != 1:
        raise ValueError("production boundary changed: " + marker)
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    # The selected methods contain no braces in string literals or comments.
    while depth and end < len(source):
        if source[end] == "{": depth += 1
        if source[end] == "}": depth -= 1
        end += 1
    if depth:
        raise ValueError("unbalanced production method: " + marker)
    return source[start:end]


def generate(component: str, ref: str | None) -> str:
    if ref:
        source = subprocess.run(["git", "show", ref + ":" + SOURCE], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout
    else:
        source = (ROOT / SOURCE).read_text()
    has_release = "  void Release() {" in source
    if component == "receiver":
        replacements = {
            "GET_CODEC": method(source, "CodecOwner GetCodec(" if "CodecOwner GetCodec(" in source else "Codec* GetCodec("),
            "SETUP": method(source, "void JNICALL MC_setup("),
            "RELEASE": method(source, "void JNICALL MC_release("),
        }
    else:
        markers = {
            "DESTRUCTOR": "  ~Codec()",
            "START": "  bool Start(std::string* error)",
            "STOP": "  void Stop()",
            "RESET": "  void Reset()",
            "FLUSH": "  void Flush()",
            "DEQUEUE_INPUT": "  int DequeueInput(int64_t timeout_us)",
            "DEQUEUE_OUTPUT": "  int DequeueOutput(JNIEnv* env, jobject info, int64_t timeout_us)",
            "STOP_WORKER": "  void StopWorker()",
        }
        replacements = {name: method(source, marker) for name, marker in markers.items()}
        replacements["RELEASE"] = method(source, "  void Release()") if has_release else ""
        replacements["STOP_LOCKED"] = method(source, "  void StopLocked()") if has_release else ""
    template = (ROOT / ("tests/media/codec_" + component + "_host.cc.in")).read_text()
    for name, body in replacements.items():
        marker = "@" + name + "@"
        if template.count(marker) != 1:
            raise ValueError("fixture boundary changed: " + marker)
        template = template.replace(marker, body)
    digest = hashlib.sha256(source.encode()).hexdigest()
    print(component + " production SHA256 " + digest, flush=True)
    return "#define HAS_RELEASE " + str(int(has_release)) + "\n" + template


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--case", choices=("receiver", "wait", "all"), default="all")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-codec-lifecycle-") as temporary:
        work = Path(temporary)
        for component in (("receiver", "wait") if args.case == "all" else (args.case,)):
            source = work / (component + ".cc")
            source.write_text(generate(component, args.source_ref))
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
