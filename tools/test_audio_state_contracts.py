#!/usr/bin/env python3
"""Run the real Track state/ring/resampler methods and SoundPool control bodies.

The host does not run WASAPI or a JNI VM. Track uses its production header and
all production methods with an Engine attachment/clock boundary; an observed
condition variable permits a deterministic pause/start before reacquisition.
SoundPool substitutes only its JNI lookup and underlying Track side effects.
The same assertions run with --source-ref as a bounded old-source control.
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


def section(text: str, start: str, end: str) -> str:
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("production section boundaries changed")
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def generate(component: str, source_ref: str | None) -> str:
    def read(path: str) -> str:
        if source_ref:
            return subprocess.run(["git", "show", source_ref + ":" + path], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / path).read_text(encoding="utf-8")
    template = (ROOT / "tests/media" / (component + "_state_host.cc.in")).read_text()
    if component == "audio":
        source = read("compat/media/audio/engine.cc")
        header = read("compat/media/audio/engine.h")
        # Visibility and the condition-variable boundary only; state/ring logic stays intact.
        adapted_header = header.replace("private:", "public:").replace("#pragma once", "")
        if adapted_header.count("std::condition_variable room_;") != 1:
            raise ValueError("Track condition variable boundary changed")
        adapted_header = adapted_header.replace("std::condition_variable room_;",
                                                "ObservedConditionVariable room_;")
        replacements = {
            "@HEADER@": adapted_header,
            "@PRIMITIVES@": section(source, "namespace gesher::media {", "// --- Engine "),
            "@TRACK@": source[source.index("Track::Track("):],
        }
        source += header
    else:
        source = read("compat/media/jni/soundpool_jni.cc")
        replacements = {
            "@STREAM@": section(source, "struct Stream {", "class Pool {"),
            "@AUTO_PAUSE@": section(source, "  void AutoPause(bool pause)", "  void Mute("),
            "@CONTROLS@": section(source, "void JNICALL SP_pause(", "void JNICALL SP_autoPause("),
        }
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
    parser.add_argument("--component", choices=["audio", "soundpool"])
    args = parser.parse_args()
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-audio-state-") as temporary:
        work = Path(temporary)
        for component in [args.component] if args.component else ["audio", "soundpool"]:
            source = work / (component + ".cc")
            source.write_text(generate(component, args.source_ref), encoding="utf-8")
            binary = work / component
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(source), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            failed |= subprocess.run([str(binary)], check=False, timeout=30).returncode != 0
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
