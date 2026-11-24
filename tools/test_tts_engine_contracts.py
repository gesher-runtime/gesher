#!/usr/bin/env python3
"""Run the text-to-speech engine contracts (docs/tts.md) on a host JVM.

The production engine (app/gesher-tts: WindowsTtsService, VoiceCatalog, WavPcm,
SpeechBackend, WindowsSpeechBackend, CheckVoiceDataActivity) and
com.gesher.server.WindowsSpeech are compiled unchanged apart from their package,
imports and public class visibility, against android-16.0.0_r4 boundary stand-ins
(tests/tts/TtsEngineContracts.java.in). Covered: languages and voices from the
Windows voice list, callback order and PCM hand-over, word ranges, rate/pitch
mapping, stop mid-utterance and during Windows startup, errors, synthesizeToFile's WAV, CHECK_TTS_DATA and the
natives' voice records. Windows itself is covered by tests/tts/tts_windows_test.cc.
Use --source-ref to run the contracts against another revision of the sources.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = "app/gesher-tts/java/com/gesher/tts/"
SOURCES = [
    (ENGINE + "SpeechBackend.java", None),
    (ENGINE + "VoiceCatalog.java", "final class VoiceCatalog"),
    (ENGINE + "WavPcm.java", "final class WavPcm"),
    (ENGINE + "WindowsSpeechBackend.java", "final class WindowsSpeechBackend"),
    (ENGINE + "WindowsTtsService.java", "public final class WindowsTtsService"),
    (ENGINE + "CheckVoiceDataActivity.java", "public final class CheckVoiceDataActivity"),
    ("app/java/com/gesher/server/WindowsSpeech.java", "public final class WindowsSpeech"),
]


def read(relative: str, ref: str | None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + relative], cwd=ROOT, text=True)
    return (ROOT / relative).read_text(encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    harness = (ROOT / "tests/tts/TtsEngineContracts.java.in").read_text(encoding="utf-8")
    inserted = []
    for relative, declaration in SOURCES:
        source = read(relative, args.source_ref)
        print(Path(relative).name, "SHA256", hashlib.sha256(source.encode()).hexdigest(), flush=True)
        # One compilation unit: the harness's imports serve all; Android types are stand-ins.
        source = re.sub(r"^package .*;\n|^import .*;\n", "", source, flags=re.MULTILINE)
        if declaration is not None:
            if declaration not in source:
                raise SystemExit(f"{relative}: expected '{declaration}'")
            source = source.replace(declaration, declaration.replace("public ", "", 1), 1)
        inserted.append(source)
    harness = harness.replace("// PRODUCTION_SOURCES", "\n".join(inserted))
    executable = lambda name: str(Path(args.java_home) / "bin" / name) if args.java_home else name
    with tempfile.TemporaryDirectory(prefix="gesher-tts-") as temporary:
        work = Path(temporary)
        java = work / "TtsEngineContracts.java"
        java.write_text(harness, encoding="utf-8")
        subprocess.run([executable("javac"), "-encoding", "UTF-8", "-Xlint:all", "-Xlint:-serial",
                        "-Werror", "-d", str(work), str(java)], check=True, timeout=120)
        subprocess.run([executable("java"), "-cp", str(work), "TtsEngineContracts"],
                       check=True, timeout=60)


if __name__ == "__main__":
    main()
