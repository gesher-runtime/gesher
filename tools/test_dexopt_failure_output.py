#!/usr/bin/env python3
"""Exercise Dexopt.failureLines, the lines of a failed dex2oat or profman run that the
system server logs, on the host JVM.

This does not run dex2oat or profman: tests/windows/tracks/61-aot2.ps1 runs them,
including with non-ASCII paths.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--framework-classes", required=True, type=Path)
    parser.add_argument("--core-classes", required=True, type=Path)
    parser.add_argument("--app-classes", required=True, type=Path)
    args = parser.parse_args()
    classpath = os.pathsep.join(str(path.resolve()) for path in (
        args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-dexopt-output-") as temporary:
        classes = Path(temporary) / "classes"
        classes.mkdir()
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes),
                        str(ROOT / "app/java/com/gesher/server/Dexopt.java"),
                        str(ROOT / "tests/package/DexoptFailureOutputHost.java")], check=True)
        subprocess.run([str(args.java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                        "com.gesher.server.DexoptFailureOutputHost"], check=True, timeout=30)


if __name__ == "__main__":
    main()
