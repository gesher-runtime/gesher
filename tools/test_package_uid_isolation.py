#!/usr/bin/env python3
"""Run production package UID admission and permission contracts with host native boundaries."""
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
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-uid-isolation-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libuid_isolation_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/location/framework_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(ROOT / "tests/dex-services/gesher/services/PackageUidIsolationContractTest.java"),
                        str(ROOT / "tests/package/PackageUidIsolationHost.java")], check=True)
        cleaner = work / "cleaner" / "sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "PackageUidIsolationHost", str(library)]
        for mode in ("0", "1"):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("data-" + mode))
            env["GESHER_MICROG_SPOOF"] = mode
            subprocess.run(command + [str(ROOT / "tests/dex-services/fixtures/v2only.apk")],
                           env=env, check=True, timeout=120)
        for state in ("unpublished", "published"):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("interruption-" + state))
            fixture = str(ROOT / "tests/dex-services/fixtures/v2only.apk")
            subprocess.run(command + [fixture, "prepare-" + state], env=env, check=True, timeout=60)
            subprocess.run(command + [fixture, "check-" + state], env=env, check=True, timeout=60)



if __name__ == "__main__":
    main()
