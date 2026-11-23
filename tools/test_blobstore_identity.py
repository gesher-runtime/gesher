#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production BlobStore ownership across attached package generations.

The fixture uses real Java services and filesystem updates with explicit host
Binder identity and package/process-record boundaries. No Windows/ART, native
revocation, remote Binder or actual APK installation is claimed.
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
    parser.add_argument("--case", choices=("all", "open"), default="all")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if compiler is None:
        parser.error("A C compiler is required for the declared native host boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-blob-identity-") as temporary:
        work = Path(temporary); classes = work / "classes"; classes.mkdir()
        library = work / "libblob_identity_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/blob_identity_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes),
                        str(ROOT / "tests/package/BlobStoreIdentityHost.java")], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "com.gesher.server.BlobStoreIdentityHost", str(library), args.case]
        for mode in ("0", "1"):
            environment = os.environ.copy()
            environment["ANDROID_DATA"] = str(work / ("data-" + mode))
            environment["GESHER_MICROG_SPOOF"] = mode
            environment.pop("GESHER_BINDER_UID", None)
            subprocess.run(command, env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
