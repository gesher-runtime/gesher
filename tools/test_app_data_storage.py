#!/usr/bin/env python3
"""Compile the exact application-storage and removal code and exercise real disk contracts."""
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
    java_home = Path(args.java_home) if args.java_home else None
    javac = str(java_home / "bin/javac") if java_home else shutil.which("javac")
    java = str(java_home / "bin/java") if java_home else shutil.which("java")
    if not javac or not java:
        parser.error("JDK 17 or newer is required (set JAVA_HOME or pass --java-home)")
    with tempfile.TemporaryDirectory(prefix="gesher-storage-contracts-") as directory:
        work = Path(directory)
        # Only platform boundaries are adapted: the production classes under test
        # are compiled byte-for-byte, and all directory/journal operations are real.
        device = work / "com/gesher/server/Device.java"
        clock = work / "android/os/SystemClock.java"
        device.parent.mkdir(parents=True)
        clock.parent.mkdir(parents=True)
        device.write_text('package com.gesher.server; public final class Device { '
                          'public static String dataDir() { '
                          'return System.getProperty("gesher.test.data"); } }\n')
        clock.write_text('package android.os; public final class SystemClock { '
                         'public static void sleep(long millis) { '
                         'try { Thread.sleep(millis); } catch (InterruptedException e) { '
                         'Thread.currentThread().interrupt(); } } }\n')
        classes = work / "classes"
        classes.mkdir()
        sources = [device, clock, ROOT / "app/java/com/gesher/server/AppDataStorage.java",
                   ROOT / "app/java/com/gesher/server/PackageRemovalStorage.java",
                   ROOT / "tests/package/AppDataStorageHostTest.java"]
        subprocess.run([javac, "-encoding", "UTF-8", "-d", str(classes),
                        *map(str, sources)], check=True)
        subprocess.run([java, "-cp", str(classes),
                        "com.gesher.server.AppDataStorageHostTest"], check=True, timeout=90)


if __name__ == "__main__":
    main()
