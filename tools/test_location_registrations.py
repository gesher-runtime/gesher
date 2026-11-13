#!/usr/bin/env python3
"""Compile and run the production location service with explicit native host boundaries.

No production Java methods are replaced. The fixture constructs package/mock
provider metadata directly and uses controlled Binder lifetimes. Its Windows
main additionally exercises real generated AIDL Parcel calls and LocationManager.
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
    parser.add_argument("--framework-classes", required=True, type=Path)
    parser.add_argument("--core-classes", required=True, type=Path)
    parser.add_argument("--app-classes", required=True, type=Path)
    parser.add_argument("--apksig-jar", required=True, type=Path)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref", help="Use old production service source as a negative control")
    parser.add_argument("--case", default="all", choices=["all", "listeners", "pending", "failures", "deaths", "current"])
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("C compiler required for host native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix="gesher-location-registrations-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        library = work / "liblocation_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/location/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/LocationManagerService.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "LocationManagerService.java"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                             cwd=ROOT, capture_output=True, check=True).stdout)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(source),
                        str(ROOT / "tests/location/LocationRegistrationHost.java"),
                        str(ROOT / "tests/dex-services/gesher/services/LocationRegistrationContractTest.java")], check=True)
        # Use the supplied Android Cleaner classes for real NativeAllocationRegistry,
        # in their owning JDK module. The Binder holder itself is the declared JNI seam.
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        cleaner_inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not cleaner_inputs:
            raise ValueError("Android core compile input has no Cleaner classes")
        for entry in cleaner_inputs:
            shutil.copy2(entry, cleaner / entry.name)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        environment["GESHER_IP_GEOLOCATION"] = "0"
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "LocationRegistrationHost", str(library), args.case],
                       check=True, env=environment, timeout=60)


if __name__ == "__main__":
    main()
