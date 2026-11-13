#!/usr/bin/env python3
"""SDK-compiled network value contracts over freshly compiled production classes.

Only native framework initialization/logging is adapted for the host JVM. The
fixture's default Windows entry point additionally requires real native Parcel
round trips. --source-ref substitutes the four repaired production classes,
leaving identical callers/dependencies to distinguish each old-source failure.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REPAIRED = {"LinkProperties", "LinkAddress", "RouteInfo", "wifi/WifiConfiguration"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--sdk-jar", type=Path, required=True)
    parser.add_argument("--framework-classes", type=Path, required=True)
    parser.add_argument("--core-classes", type=Path, required=True)
    parser.add_argument("--app-classes", type=Path, required=True)
    parser.add_argument("--source-ref", help="Compile older production sources as a negative control")
    parser.add_argument("--group", choices=("all", "addresses", "routes", "values", "nat64", "proxies"),
                        default="all")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the declared native initialization boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    with tempfile.TemporaryDirectory(prefix="gesher-network-snapshot-contracts-") as temporary:
        work = Path(temporary)
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir()
        callers.mkdir()
        library = work / "libnetwork_value_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home.resolve() / "include"),
                        "-I" + str(args.java_home.resolve() / "include/linux"),
                        str(ROOT / "tests/component-caller/framework_host.c"), "-o", str(library)],
                       check=True)
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", str(args.sdk_jar.resolve()), "-d", str(callers),
                        str(ROOT / "tests/dex-services/gesher/services/NetworkSnapshotContractTest.java")],
                       check=True)
        sources = []
        for name in ("LinkProperties", "LinkAddress", "RouteInfo", "wifi/WifiConfiguration",
                     "IpPrefix", "IpConfiguration", "StaticIpConfiguration", "ProxyInfo", "InetAddresses"):
            relative = "app/java/android/net/" + name + ".java"
            source = ROOT / relative
            if args.source_ref and name in REPAIRED:
                source = work / (Path(name).name + ".java")
                source.write_bytes(subprocess.run(
                    ["git", "show", args.source_ref + ":" + relative], cwd=ROOT,
                    capture_output=True, check=True).stdout)
            sources.append(str(source))
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), *sources], check=True)
        subprocess.run([java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions",
                        "-XX:-BytecodeVerificationRemote", "-cp",
                        os.pathsep.join((str(classes), str(callers), classpath)),
                        "gesher.services.NetworkSnapshotContractTest", str(library), args.group],
                       check=True, timeout=60)
        print("SDK 36 callers and actual production network values; host initialization/logging "
              "boundary only. Native Parcel is a separate Windows gate.")


if __name__ == "__main__":
    main()
