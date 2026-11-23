#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run actual PackageInstaller ownership methods with controlled host Binder identities.

--app-classes may name an unmodified baseline. --case open independently checks
replacement-session access without first stopping at the creation regression.
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
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--case", choices=("all", "open"), default="all")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if compiler is None:
        parser.error("A C compiler is required for native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-installer-identity-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libinstaller_identity_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/installer_identity_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes),
                        str(ROOT / "tests/package/PackageInstallerIdentityHost.java")], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "com.gesher.server.PackageInstallerIdentityHost", str(library), args.case]
        for mode in ("0", "1"):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("data-" + mode))
            env["GESHER_MICROG_SPOOF"] = mode
            env.pop("GESHER_BINDER_UID", None)
            subprocess.run(command, env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
