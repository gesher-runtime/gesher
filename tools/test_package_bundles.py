#!/usr/bin/env python3
"""Compile and run the production APK bundle classification and selection on real ZIP files."""
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
    with tempfile.TemporaryDirectory(prefix="gesher-package-bundles-") as directory:
        work = Path(directory)
        sources = [str(ROOT / "app/java/com/gesher/app" / name)
                   for name in ("PackageBundles.java", "ApkSetToc.java", "NativeLibs.java")]
        subprocess.run([javac, "--release", "17", "-d", str(work / "classes"), *sources,
                        str(ROOT / "tests/package/PackageBundlesTest.java")], check=True)
        subprocess.run([java, "-cp", str(work / "classes"), "com.gesher.app.PackageBundlesTest",
                        str(work / "data")], check=True, timeout=90)


if __name__ == "__main__":
    main()
