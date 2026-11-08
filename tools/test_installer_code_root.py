#!/usr/bin/env python3
"""Exercise actual Installer/ApkPackage guards with real host filesystem aliases and links.

This does not execute ART, parse APKs, or prove Windows path normalization. The real
SplitPackageContractTest separately installs, reloads, and loads APK split code on Windows.
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
    parser.add_argument("--apksig", required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile Installer at this ref as a negative control")
    args = parser.parse_args()
    classpath = os.pathsep.join(str(path.resolve()) for path in (
        args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-code-root-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        source = ROOT / "app/java/com/gesher/server/Installer.java"
        if args.source_ref:
            previous = subprocess.run(
                ["git", "show", args.source_ref + ":app/java/com/gesher/server/Installer.java"],
                cwd=ROOT, capture_output=True, check=True)
            source = work / "Installer.java"
            source.write_bytes(previous.stdout)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(source),
                        str(ROOT / "app/java/com/gesher/server/ApkPackage.java"),
                        str(ROOT / "tests/package/InstallerCodeRootHost.java")], check=True)
        data = work / "data"
        data.mkdir()
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(data) + "/../data"
        subprocess.run([str(args.java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                        "InstallerCodeRootHost"], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
