#!/usr/bin/env python3
"""Exercise actual install-source policy/registry with verified APKs and host boundaries."""
from __future__ import annotations

import argparse
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def exercise(args: argparse.Namespace, work: Path, compiler: str) -> None:
    work.mkdir(parents=True, exist_ok=True)
    classes = work / "classes"
    classes.mkdir()
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    library = work / "libinstall_source_host.so"
    commands = [
        [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
         "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
         str(ROOT / "tests/location/framework_host.c"), "-o", str(library)],
        [str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
         "-cp", classpath, "-d", str(classes), str(ROOT / "tests/package/InstallSourceHost.java")],
    ]
    for command in commands:
        subprocess.run(command, check=True, timeout=120)
    cleaner = work / "cleaner/sun/misc"
    cleaner.mkdir(parents=True)
    for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
        shutil.copy2(source, cleaner / source.name)
    command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
               "jdk.unsupported=" + str(work / "cleaner"), "-cp",
               str(classes) + os.pathsep + classpath, "com.gesher.server.InstallSourceHost",
               str(library.resolve()), str(ROOT / "tests/dex-services/fixtures")]
    for mode in ("0", "1"):
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str((work / ("data-" + mode)).resolve())
        environment["GESHER_MICROG_SPOOF"] = mode
        environment.pop("GESHER_BINDER_UID", None)
        subprocess.run(command, env=environment, check=True, timeout=90)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig"):
        parser.add_argument("--" + key, required=True, type=Path)
    parser.add_argument("--output", type=Path, help="Retain evidence in a new or empty directory")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if compiler is None:
        parser.error("A C compiler is required for native host boundaries")
    if args.output is not None and args.output.exists() and any(args.output.iterdir()):
        parser.error("--output must name a new or empty directory")
    owner = contextlib.nullcontext(str(args.output.resolve())) if args.output is not None else \
            tempfile.TemporaryDirectory(prefix="gesher-install-source-")
    with owner as directory:
        exercise(args, Path(directory), compiler)


if __name__ == "__main__":
    main()
