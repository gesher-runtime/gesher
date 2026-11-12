#!/usr/bin/env python3
"""Run actual ZIP extraction, using production NativeLibs on the host JDK.

The same Java fixture also runs on Windows ART. --source-ref selects the original
production source to show that unsafe Windows names were accepted before repair.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-native-library-paths-") as temporary:
        work = Path(temporary)
        relative = "app/java/com/gesher/app/NativeLibs.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "NativeLibs.java"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                              cwd=ROOT, capture_output=True, check=True).stdout)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-d", str(work),
                        str(source), str(ROOT / "tests/dex-services/gesher/services/NativeLibraryPathsTest.java")],
                       check=True, timeout=60)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-cp", str(work),
                        "gesher.services.NativeLibraryPathsTest"], check=True, timeout=30)


if __name__ == "__main__":
    main()
