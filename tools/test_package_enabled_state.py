#!/usr/bin/env python3
"""Exercise public package enabled-state and, in the full suite, searchable visibility.

For enabled-state negative controls, --app-classes can point at an unmodified
baseline because those fixtures call only public APIs.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_searchable import run_searchable

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--group", choices=("all", "application", "metadata", "batch", "cache", "validation", "persistence", "quarantine", "lifecycle", "generation", "standalone", "installation"), default="all")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-enabled-state-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libenabled_state_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/enabled_state_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(ROOT / "tests/dex-services/gesher/services/PackageEnabledStateContractTest.java"),
                        str(ROOT / "tests/package/PackageEnabledStateHost.java")], check=True)
        cleaner = work / "cleaner" / "sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "PackageEnabledStateHost", str(library)]
        for mode in ("0", "1"):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("data-" + mode))
            env["GESHER_MICROG_SPOOF"] = mode
            subprocess.run(command + [args.group], env=env, check=True, timeout=120)
            if args.group == "all":
                subprocess.run(command + ["write"], env=env, check=True, timeout=90)
                subprocess.run(command + ["read"], env=env, check=True, timeout=90)

    if args.group == "all":
        run_searchable(java_home=args.java_home, classpath=classpath, cc=cc)


if __name__ == "__main__":
    main()
