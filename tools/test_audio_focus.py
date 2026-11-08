#!/usr/bin/env python3
"""Run production audio focus transitions with real framework classes and host JNI boundaries.

The common fixture controls Binder lifetime and unrelated stream-volume initialization.
Its Windows main separately exercises generated AIDL Parcel and public AudioManager callbacks.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--framework-classes", type=Path, required=True)
    parser.add_argument("--core-classes", type=Path, required=True)
    parser.add_argument("--app-classes", type=Path, required=True)
    parser.add_argument("--source-ref", help="Compile an older production service as a negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the test-only JNI boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-audio-focus-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libaudio_focus_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"),
                        "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/audio/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/AudioService.java"
        service = ROOT / relative
        if args.source_ref:
            old = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                 cwd=ROOT, capture_output=True, check=True)
            service = work / "AudioService.java"
            service.write_bytes(old.stdout)
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData",
                        "-J-Djava.io.tmpdir=" + str(work), "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), str(service),
                        str(ROOT / "tests/dex-services/gesher/services/AudioFocusLifecycleTest.java"),
                        str(ROOT / "tests/audio/AudioFocusHost.java")], check=True)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-Djava.io.tmpdir=" + str(work),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "AudioFocusHost", str(library)], check=True, timeout=90)


if __name__ == "__main__":
    main()
