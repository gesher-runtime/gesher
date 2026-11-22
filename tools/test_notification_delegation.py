#!/usr/bin/env python3
"""Run actual notification delegation policy with controlled host native boundaries."""
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
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--signed-apk", type=Path, help="Real signed APK for verified continuity check")
    parser.add_argument("--service-source-ref", help="Prior notification service as a negative control")
    parser.add_argument("--with-storage", action="store_true",
                        help="Also execute retained attribution using the explicit host Parcel serialization seam")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc: parser.error("A C compiler is required")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                              (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-notification-delegation-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libnotification_delegation_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/uri/intent_framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("NotificationManagerService", "NotificationDelegates", "NotificationStore"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if name == "NotificationManagerService" and args.service_source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.run(["git", "show", args.service_source_ref + ":" + relative],
                                                 cwd=ROOT, check=True, capture_output=True).stdout)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/FixtureServiceContext.java"),
                    str(ROOT / "tests/dex-services/gesher/services/NotificationDelegationTest.java"),
                    str(ROOT / "tests/notification-delegation/NotificationDelegationHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        if args.with_storage:
            subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                            "-d", str(classes), str(ROOT / "tests/notification-identity/Parcel.java")], check=True)
        # Real PermissionPolicy initializes AppOpsManager's local Binder callback.
        # Reuse the declared holder JNI boundary and the supplied Android Cleaner
        # classes; do not replace Binder, NativeAllocationRegistry or AppOps policy.
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        cleaner_inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not cleaner_inputs:
            raise ValueError("Android core compile input has no Cleaner classes")
        for entry in cleaner_inputs:
            shutil.copy2(entry, cleaner / entry.name)
        for protected in (True, False):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("protected" if protected else "legacy"))
            env["GESHER_MICROG_SPOOF"] = "1" if protected else "0"
            env.pop("GESHER_BINDER_UID", None)
            options = (["-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                        "-Dgesher.fixture.notification.storage=true"] if args.with_storage else [])
            command = [str(args.java_home / "bin/java"), *options,
                       "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                       "-cp", str(classes) + os.pathsep + classpath,
                       "NotificationDelegationHost", str(library)]
            if args.signed_apk: command.append(str(args.signed_apk.resolve()))
            subprocess.run(command, env=env, check=True, timeout=90)


if __name__ == "__main__": main()
