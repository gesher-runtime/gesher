#!/usr/bin/env python3
"""Exercise the production audio buffer adapters with a real host JVM/JNI.

The eight functions and their native registrations are copied verbatim. Only
WriteBytes/ReadInto (the Windows audio engines) are replaced with a bounded
transfer observer. This tests JNI/buffer contracts, not actual audio output,
capture, ART or either Windows guest ABI. --track-source/--record-source can
select old source files for negative controls in disposable JVM processes.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def extract(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("audio source boundaries changed")
    return source[source.index(start):source.index(end, source.index(start))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--track-source", type=Path,
                        default=ROOT / "compat/media/jni/audio_track_jni.cc")
    parser.add_argument("--record-source", type=Path,
                        default=ROOT / "compat/media/jni/audio_record_jni.cc")
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    java_home = args.java_home
    if java_home is None:
        javac = shutil.which("javac")
        if javac is None:
            parser.error("a JDK is required (JAVA_HOME or --java-home)")
        java_home = Path(javac).resolve().parent.parent
    track = args.track_source.read_text(encoding="utf-8")
    record = args.record_source.read_text(encoding="utf-8")
    sections = [extract(track, "jint JNICALL AudioTrack_writeByte(",
                        "jint JNICALL AudioTrack_reloadStatic("),
                extract(record, "jint JNICALL AR_readBytes(",
                        "jint JNICALL AR_bufferSizeInFrames(")]
    names = ("AudioTrack_writeByte", "AudioTrack_writeShort", "AudioTrack_writeFloat",
             "AudioTrack_writeNativeBytes", "AR_readBytes", "AR_readShorts",
             "AR_readFloats", "AR_readDirect")
    entries = []
    for name in names:
        matches = re.findall(r'\{"[^"\n]+",\s*"[^"\n]+",\s*GESHER_NATIVE\('
                             + re.escape(name) + r'\)\}', track + record)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous JNI registration: " + name)
        entries.append(matches[0])
    template = (ROOT / "tests/media/audio_buffer_host.cc.in").read_text(encoding="utf-8")
    for marker, content in (("@BUFFER_FUNCTIONS@", "\n".join(sections)),
                            ("@NATIVE_REGISTRATIONS@", ",\n".join(entries))):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous host marker: " + marker)
        template = template.replace(marker, content)
    for label, source in (("track", track), ("record", record)):
        print(label + " source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-audio-buffers-") as tmp:
        folder = Path(tmp)
        cpp = folder / "audio_buffers.cc"
        cpp.write_text(template, encoding="utf-8")
        library = folder / "libaudio_buffers.so"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-write-strings", "-shared", "-fPIC",
            "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
            "-I", str(java_home / "include"),
            "-I", str(java_home / "include/linux"), str(cpp), "-o", str(library)]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(java_home / "bin/javac"), "-d", str(folder),
                        str(ROOT / "tests/media/AudioBufferContractTest.java")],
                       check=True, timeout=60)
        for case in ("valid", "record-capacity", "record-bounds", "track-bounds",
                     "direct-bounds", "null-direct"):
            result = subprocess.run([
                str(java_home / "bin/java"), "-Xcheck:jni", "-XX:-CreateCoredumpOnCrash",
                "-cp", str(folder), "AudioBufferContractTest", str(library), case],
                cwd=folder, capture_output=True, text=True, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:6000]}", flush=True)
        if failed:
            print("Failing audio buffer contracts: " + ", ".join(failed))
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
