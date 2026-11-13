#!/usr/bin/env python3
"""Exercise production network builders with SDK-only callers and host boundaries.

--source-ref replaces the existing NetworkCapabilities/NetworkRequest sources;
new complete value dependencies remain available to isolate behavior regressions.
Native Parcel assertions run separately in the Windows ART fixtures.
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
    for option in ("framework-classes", "core-classes", "app-classes", "sdk-jar", "java-home"):
        parser.add_argument("--" + option, required=True, type=Path)
    parser.add_argument("--source-ref")
    parser.add_argument("--case", default="all", choices=["all", "bounds", "specifiers", "legacy", "ethernet", "raw", "enterprise", "values"])
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("C compiler required for the declared host initialization boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-network-builder-") as temporary:
        work = Path(temporary)
        production = work / "production"; production.mkdir()
        callers = work / "callers"; callers.mkdir()
        native = work / "libnetwork_builder_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/network-builder/framework_host.c"), "-o", str(native)], check=True)
        sources = []
        for name in ("NetworkCapabilities.java", "NetworkRequest.java", "EthernetNetworkSpecifier.java", "TestNetworkSpecifier.java"):
            relative = "app/java/android/net/" + name
            source = ROOT / relative
            if args.source_ref and name in ("NetworkCapabilities.java", "NetworkRequest.java"):
                source = work / name
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                                 cwd=ROOT, capture_output=True, check=True).stdout)
            sources.append(str(source))
        javac = str(args.java_home / "bin/javac")
        subprocess.run([javac, "-encoding", "UTF-8", "-cp", classpath, "-d", str(production), *sources], check=True)
        fixture = ROOT / "tests/dex-services/gesher/services"
        # No runtime classes are on this SDK-only caller's compilation classpath.
        subprocess.run([javac, "-encoding", "UTF-8", "-cp", str(args.sdk_jar.resolve()),
                        "-d", str(callers), str(fixture / "NetworkBuilderContractTest.java")], check=True)
        runtime = os.pathsep.join((str(production), str(callers), classpath))
        subprocess.run([javac, "-encoding", "UTF-8", "-cp", runtime, "-d", str(callers),
                        str(fixture / "NetworkBuilderInternalContractTest.java"),
                        str(ROOT / "tests/network-builder/NetworkBuilderHost.java")], check=True)
        subprocess.run([str(args.java_home / "bin/java"), "-cp", runtime,
                        "NetworkBuilderHost", str(native), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
