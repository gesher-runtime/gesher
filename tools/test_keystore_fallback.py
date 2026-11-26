#!/usr/bin/env python3
"""Run the production software keystore with only the app data directory supplied by a fixture."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/biometrics/keystore"


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
        for phase in ("seed", "failed-persistence", "migrate", "reload"):
            subprocess.run(
                [str(java_bin / "java"), "-cp", os.pathsep.join((str(classes), android)),
                 "com.gesher.tests.AndroidKeyStoreContracts", phase, str(work)],
                check=True, timeout=60
            )


if __name__ == "__main__":
    main()
