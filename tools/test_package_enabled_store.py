#!/usr/bin/env python3
"""Compile and run the exact production enabled-state store with real filesystem failures."""
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
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    java = str(args.java_home / "bin/java") if args.java_home else shutil.which("java")
    javac = str(args.java_home / "bin/javac") if args.java_home else shutil.which("javac")
    if not java or not javac:
        parser.error("JDK 17+ is required")
    with tempfile.TemporaryDirectory(prefix="gesher-enabled-store-") as directory:
        work = Path(directory)
        subprocess.run([javac, "--release", "17", "-d", str(work / "classes"),
                        str(ROOT / "app/java/com/gesher/server/PackageEnabledStore.java"),
                        str(ROOT / "tests/package/PackageEnabledStoreTest.java")], check=True)
        subprocess.run([java, "-cp", str(work / "classes"), "com.gesher.server.PackageEnabledStoreTest",
                        str(work / "data")], check=True, timeout=90)


if __name__ == "__main__":
    main()
