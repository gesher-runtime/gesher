#!/usr/bin/env python3
"""Run the speech recognition service contracts (docs/speech-recognition.md) on a host JVM.

The production engine (app/gesher-stt: WindowsRecognitionService, RecognizerBackend,
WindowsRecognizerBackend, AudioInput, MicrophoneInput, StreamInput, Languages) and
com.gesher.server.WindowsSpeechRecognition are compiled unchanged apart from their
package, imports and public class visibility, against android-16.0.0_r4 boundary
stand-ins (tests/speech/SpeechRecognitionContracts.java.in), whose RecognitionService
follows AOSP's dispatchStartListening: busy, RECORD_AUDIO preflight, onStartListening,
data delivery, ERROR_INSUFFICIENT_PERMISSIONS and onCancel. Covered: language matching
against Windows' recognizer languages, callback order, partial results, confidences,
permission refusals before any microphone, language and start errors, audio failures,
speech time-out, no match, stopListening, cancel, onDestroy, busy, client death,
EXTRA_AUDIO_SOURCE (with pipe reads of half a sample and an odd trailing byte),
checkRecognitionSupport, triggerModelDownload and levels. Windows
itself is covered by tests/speech/speech_recognition_windows_test.cc.
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
ENGINE = "app/gesher-stt/java/com/gesher/stt/"
SOURCES = [
    (ENGINE + "RecognizerBackend.java", None),
    (ENGINE + "AudioInput.java", None),
    (ENGINE + "Languages.java", "final class Languages"),
    (ENGINE + "MicrophoneInput.java", "final class MicrophoneInput"),
    (ENGINE + "StreamInput.java", "final class StreamInput"),
    (ENGINE + "WindowsRecognizerBackend.java", "final class WindowsRecognizerBackend"),
    (ENGINE + "WindowsRecognitionService.java", "public final class WindowsRecognitionService"),
    ("app/java/com/gesher/server/WindowsSpeechRecognition.java",
     "public final class WindowsSpeechRecognition"),
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
    harness = (ROOT / "tests/speech/SpeechRecognitionContracts.java.in").read_text(encoding="utf-8")
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
    with tempfile.TemporaryDirectory(prefix="gesher-speech-") as temporary:
        work = Path(temporary)
        java = work / "SpeechRecognitionContracts.java"
        java.write_text(harness, encoding="utf-8")
        subprocess.run([executable("javac"), "-encoding", "UTF-8", "-Xlint:all", "-Xlint:-serial",
                        "-Werror", "-d", str(work), str(java)], check=True, timeout=120)
        subprocess.run([executable("java"), "-cp", str(work), "SpeechRecognitionContracts"],
                       check=True, timeout=180)


if __name__ == "__main__":
    main()
