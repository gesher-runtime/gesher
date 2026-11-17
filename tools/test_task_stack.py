#!/usr/bin/env python3
"""Run production task-stack/PendingIntent contracts using real framework classes.

The host adapts clock/Binder identity natives only. ART and Parcel transport remain
the separate mandatory Windows fixture. --source-ref compiles the original
production classes against the same independent assertions as a negative control.
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
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref")
    parser.add_argument("--case", choices=("all", "history", "validation", "tasks", "attach", "visibility", "permissions", "pending"), default="all")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        parser.error("A C compiler is required for the existing native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-task-stack-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libtask_stack_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("ActivityTaskManagerService", "ActivityManagerService", "PendingIntents"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                        cwd=ROOT, capture_output=True, check=True).stdout)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/TaskStackContractTest.java"),
                    str(ROOT / "tests/activity-finish/TaskStackHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not inputs:
            raise ValueError("Android core classes must include their Cleaner implementation")
        for source in inputs:
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy()
        env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_MICROG_SPOOF"] = "1"
        env["GESHER_LOCALES"] = "en-US"
        env.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "TaskStackHost", str(library), args.case], env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
