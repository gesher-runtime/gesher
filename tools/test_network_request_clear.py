#!/usr/bin/env python3
"""Run the real NetworkRequest builder and NetworkCapabilities against framework classes."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework-classes", required=True, type=Path)
    parser.add_argument("--core-classes", required=True, type=Path)
    parser.add_argument("--app-classes", required=True, type=Path)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref", help="Use old production sources as a negative control")
    args = parser.parse_args()
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-network-request-clear-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        sources = []
        for name in ("NetworkRequest.java", "NetworkCapabilities.java"):
            relative = "app/java/android/net/" + name
            source = ROOT / relative
            if args.source_ref:
                source = work / name
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                                 cwd=ROOT, capture_output=True, check=True).stdout)
            sources.append(str(source))
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources,
                        str(ROOT / "tests/dex-services/gesher/services/NetworkRequestClearContractTest.java")], check=True)
        subprocess.run([str(args.java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                        "gesher.services.NetworkRequestClearContractTest"], check=True, timeout=30)


if __name__ == "__main__":
    main()
