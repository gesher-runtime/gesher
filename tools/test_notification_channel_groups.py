#!/usr/bin/env python3
"""Execute actual notification service/framework Java on a host JDK.

Requires verified framework/coremod compilation inputs and an app compile-classes
jar from app/build.sh. Recompiles the production sources, not a rewritten model.
The Windows NotificationChannelGroupsTest exercises real Binder/Parcel and posting;
this diagnostic uses explicitly separate native-boundary adapters.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["NotificationManagerService.java", "NotificationStore.java",
           "ActiveServices.java", "ActivityManagerService.java", "JobSchedulerService.java"]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--framework-classes", required=True, type=Path)
    ap.add_argument("--core-classes", required=True, type=Path)
    ap.add_argument("--app-classes", required=True, type=Path)
    ap.add_argument("--java-home", type=Path)
    ap.add_argument("--source-ref", help="Compile production sources at this git ref as a negative control")
    args = ap.parse_args()
    java_home = args.java_home or Path(os.environ.get("JAVA_HOME", ""))
    if not (java_home / "bin/javac").is_file():
        javac = shutil.which("javac")
        if not javac:
            ap.error("A JDK is required (--java-home or JAVA_HOME)")
        java_home = Path(javac).resolve().parent.parent
    cc = shutil.which("cc")
    if cc is None:
        ap.error("A C compiler is required for the fixture's JNI boundary adapters")
    classpath = os.pathsep.join(str(p.resolve()) for p in
            [args.app_classes, args.framework_classes, args.core_classes])
    with tempfile.TemporaryDirectory(prefix="gesher-notification-groups-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libnotification_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(java_home / "include"), "-I" + str(java_home / "include/linux"),
                        str(ROOT / "tests/notification/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in SOURCES:
            relative = "app/java/com/gesher/server/" + name
            source = ROOT / relative
            if args.source_ref:
                source = work / name
                before = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                        cwd=ROOT, capture_output=True, check=True)
                source.write_bytes(before.stdout)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/NotificationChannelGroupsTest.java"),
                    str(ROOT / "tests/notification/NotificationGroupsHost.java")]
        subprocess.run([str(java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        environment.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                        "NotificationGroupsHost", str(library)], env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
