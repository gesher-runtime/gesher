#!/usr/bin/env python3
"""Run SDK-compiled Bucket call sites against the actual production implementation.

No network-accounting service is provided by this test. The existing Windows
NetworkStatsUnavailableTest checks service lookup and query failure separately.
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
    parser.add_argument("--sdk-jar", required=True, type=Path)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--source-ref", help="Compile old production source as a negative control")
    args = parser.parse_args()
    java_home = args.java_home or Path(os.environ.get("JAVA_HOME", ""))
    if not (java_home / "bin/javac").is_file():
        javac = shutil.which("javac")
        if not javac:
            parser.error("A JDK is required (--java-home or JAVA_HOME)")
        java_home = Path(javac).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gesher-network-stats-bucket-") as directory:
        work = Path(directory)
        production = work / "production"
        callers = work / "callers"
        production.mkdir()
        callers.mkdir()
        relative = "app/java/android/app/usage/NetworkStats.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "NetworkStats.java"
            source.write_bytes(subprocess.run(
                ["git", "show", args.source_ref + ":" + relative], cwd=ROOT,
                capture_output=True, check=True).stdout)
        subprocess.run([str(java_home / "bin/javac"), "-d", str(production), str(source)], check=True)
        subprocess.run([str(java_home / "bin/javac"), "-cp", str(args.sdk_jar.resolve()),
                        "-d", str(callers), str(ROOT / "tests/dex-services/gesher/services/"
                                               "NetworkStatsBucketContractTest.java")], check=True)
        subprocess.run([str(java_home / "bin/java"), "-cp",
                        os.pathsep.join([str(production), str(callers)]),
                        "gesher.services.NetworkStatsBucketContractTest"], check=True, timeout=30)


if __name__ == "__main__":
    main()
