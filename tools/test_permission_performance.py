#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Count production permission/identity/catalogue work with explicit host native boundaries.

Pass freshly built app classes. --case normal/signer/startup also runs against an
unmodified pre-optimization build as a failing negative control. Counts measure
specific operations, not Windows launch time or end-to-end application speed.
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
    parser.add_argument("--case", choices=("all", "normal", "signer", "startup", "catalog"), default="all")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if compiler is None:
        parser.error("A C compiler is required for the host native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-permission-performance-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libpermission_performance_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/appops/framework_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes),
                        str(ROOT / "tests/package/PermissionPerformanceHost.java")], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data")
        env.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                        "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                        "com.gesher.server.PermissionPerformanceHost", str(library), args.case],
                       env=env, check=True, timeout=60)


if __name__ == "__main__":
    main()
