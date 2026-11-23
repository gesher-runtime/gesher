#!/usr/bin/env python3
"""Exercise the production callback registry with deterministic service/scheduler boundaries.

The Windows fixture additionally requires actual Binder Parcel and HandlerThread
execution. No production method is replaced. The fixture injects the declared
service and scheduling objects into the existing registry through reflection.
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
    for option in ("framework-classes", "core-classes", "app-classes", "java-home"):
        parser.add_argument("--" + option, required=True, type=Path)
    # The existing framework job verifies/downloads this same input before the
    # scheduler and connectivity fixtures. Local runs can name it explicitly.
    apksig = os.environ.get("APKSIG_JAR")
    if not apksig and os.environ.get("RUNNER_TEMP"):
        apksig = str(Path(os.environ["RUNNER_TEMP"]) / "gesher-apksig.jar")
    parser.add_argument("--apksig-jar", type=Path, default=Path(apksig) if apksig else None)
    parser.add_argument("--source-ref")
    parser.add_argument("--cc", help="Host C compiler (llvm-mingw on Windows)")
    parser.add_argument("--case", default="all", choices=["all", "basic", "identity", "timeouts", "teardown", "subscriptions", "pending", "pending-cancel", "freshness", "demand", "idle-trace"])
    parser.add_argument("--trace-ms", type=int, default=600_000, help="No-owner trace duration (idle-trace case only)")
    args = parser.parse_args()
    cc = args.cc or shutil.which("cc")
    if not cc:
        parser.error("C compiler required for host native initialization boundaries")
    if args.case in ("all", "demand") and (not args.apksig_jar or not args.apksig_jar.is_file()):
        parser.error("--apksig-jar (or APKSIG_JAR/the framework job's RUNNER_TEMP input) is required for owner metadata")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    if args.apksig_jar:
        classpath += os.pathsep + str(args.apksig_jar.resolve())
    with tempfile.TemporaryDirectory(prefix="gesher-connectivity-callback-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        windows = os.name == 'nt'
        native = work / ('connectivity_callback_host.dll' if windows else 'libconnectivity_callback_host.so')
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DGESHER_CONNECTIVITY_DEMAND", "-shared", *([] if windows else ['-fPIC']),
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / 'include' / ('win32' if windows else 'linux')),
                        str(ROOT / "tests/connectivity-callback/framework_host.c"), "-o", str(native),
                        *(['-lwinpthread', '-static'] if windows else [])], check=True)
        sources = []
        for relative in ("app/java/android/net/ConnectivityManager.java",
                         "app/java/com/gesher/server/ConnectivityService.java",
                         "app/java/com/gesher/server/StickyBroadcasts.java",
                         "app/java/com/gesher/server/BroadcastController.java",
                         "app/java/com/gesher/server/JobSchedulerService.java"):
            source = ROOT / relative
            if args.source_ref:
                source = work / source.name
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                                 cwd=ROOT, capture_output=True, check=True).stdout)
            sources.append(str(source))
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", classpath,
                        "-sourcepath", str(ROOT / "tests/dex-services"), "-d", str(classes), *sources,
                        str(ROOT / "tests/connectivity-callback/ConnectivityCallbackHost.java"),
                        str(ROOT / "tests/connectivity-callback/ConnectivityServiceSnapshotHost.java"),
                        str(ROOT / "tests/connectivity-callback/ConnectivityDemandHost.java"),
                        str(ROOT / "tests/dex-services/gesher/services/JobSchedulerConstraintContractTest.java"),
                        str(ROOT / "tests/dex-services/gesher/services/JobSchedulerAdmissionTest.java"),
                        str(ROOT / "tests/dex-services/gesher/services/JobSchedulerContentTriggerTest.java"),
                        str(ROOT / "tests/dex-services/gesher/services/ConnectivityCallbackContractTest.java")], check=True)
        # Android NativeAllocationRegistry uses its Cleaner in jdk.unsupported.
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not inputs:
            raise ValueError("Android core compile input has no Cleaner classes")
        for entry in inputs:
            shutil.copy2(entry, cleaner / entry.name)
        environment = os.environ.copy()
        environment['ANDROID_DATA'] = str(work / 'data')
        environment.pop('GESHER_BINDER_UID', None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "ConnectivityCallbackHost", str(native), args.case, str(args.trace_ms)], check=True, env=environment,
                       timeout=45 + (args.trace_ms / 1000 if args.case == "idle-trace" else 0))


if __name__ == "__main__":
    main()
