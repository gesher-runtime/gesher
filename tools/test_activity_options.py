#!/usr/bin/env python3
"""Run actual PackageManagerService/activity-options/framework Java with host JNI boundaries.

This does not execute ART, Binder Parcel or a real parsed APK. Those are exercised by
ActivityOptionsContractTest's separate Windows entry point with components.apk.
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
    parser.add_argument("--apksig", required=True, type=Path)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile PMS at this ref as a negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is needed for the existing host JNI boundary adapters")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            [args.app_classes, args.framework_classes, args.core_classes, args.apksig])
    with tempfile.TemporaryDirectory(prefix="gesher-activity-options-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libactivity_options_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"),
                        "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/PackageManagerService.java"
        service = ROOT / relative
        if args.source_ref:
            service = work / "PackageManagerService.java"
            before = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                    cwd=ROOT, capture_output=True, check=True)
            service.write_bytes(before.stdout)
        sources = [str(service), str(ROOT / "app/java/com/gesher/server/ActivityOptionsResolver.java"),
                   str(ROOT / "tests/dex-services/gesher/services/ActivityOptionsContractTest.java"),
                   str(ROOT / "tests/package/ActivityOptionsHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        subprocess.run([str(args.java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                        "ActivityOptionsHost", str(library)], env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
