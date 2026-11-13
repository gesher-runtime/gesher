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
    parser.add_argument("--source-ref")
    parser.add_argument("--case", default="all", choices=["all", "basic", "identity", "timeouts", "teardown"])
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("C compiler required for host native initialization boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-connectivity-callback-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        native = work / "libconnectivity_callback_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/connectivity-callback/framework_host.c"), "-o", str(native)], check=True)
        relative = "app/java/android/net/ConnectivityManager.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "ConnectivityManager.java"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                             cwd=ROOT, capture_output=True, check=True).stdout)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(source),
                        str(ROOT / "tests/connectivity-callback/ConnectivityCallbackHost.java"),
                        str(ROOT / "tests/dex-services/gesher/services/ConnectivityCallbackContractTest.java")], check=True)
        # Android NativeAllocationRegistry uses its Cleaner in jdk.unsupported.
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not inputs:
            raise ValueError("Android core compile input has no Cleaner classes")
        for entry in inputs:
            shutil.copy2(entry, cleaner / entry.name)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "ConnectivityCallbackHost", str(native), args.case], check=True, timeout=45)


if __name__ == "__main__":
    main()
