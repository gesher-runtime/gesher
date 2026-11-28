#!/usr/bin/env python3
"""Run the production software keystore with only the app data directory supplied by a fixture."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/biometrics/keystore"


def check_process_lock(command, directory):
    directory.mkdir()
    subprocess.run([*command, "race-seed", str(directory)], check=True, timeout=60)
    children = []
    try:
        for phase in ("race-first", "race-second"):
            children.append(subprocess.Popen([*command, phase, str(directory)]))
        deadline = time.monotonic() + 6
        while not (directory / "second-attempt").exists():
            if time.monotonic() >= deadline or any(child.poll() is not None for child in children):
                raise AssertionError("keystore writers did not reach the race barrier")
            time.sleep(0.01)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if (directory / "second-finished").exists():
                raise AssertionError("a second process published while the first held the store lock")
            time.sleep(0.01)
        (directory / "release-first").touch()
        for child in children:
            if child.wait(timeout=15) != 0:
                raise AssertionError("concurrent AndroidKeyStore writer failed")
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=15)
    subprocess.run([*command, "race-check", str(directory)], check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--framework-classes", type=Path, required=True)
    parser.add_argument("--core-classes", type=Path, required=True)
    args = parser.parse_args()

    java_bin = args.java_home / "bin"
    with tempfile.TemporaryDirectory(prefix="gesher-software-keystore-") as name:
        work = Path(name)
        classes = work / "classes"
        android = os.pathsep.join(map(str, (args.framework_classes, args.core_classes)))
        sources = [
            ROOT / "app/java/com/gesher/app/AndroidKeyStore.java",
            *FIXTURE.rglob("*.java"),
        ]
        subprocess.run(
            [str(java_bin / "javac"), "-encoding", "UTF-8", "-cp", android, "-d", str(classes),
             *map(str, sources)], check=True, timeout=60
        )
        command = [str(java_bin / "java"), "-cp", os.pathsep.join((str(classes), android)),
                   "com.gesher.tests.AndroidKeyStoreContracts"]
        for phase in ("seed", "reject-unarchived", "seed-scrubbed-fixture",
                      "failed-persistence", "migrate", "reload", "recursive"):
            subprocess.run(
                [*command, phase, str(work)], check=True, timeout=60
            )
        check_process_lock(command, work / "second-process")


if __name__ == "__main__":
    main()
