#!/usr/bin/env python3
"""Run real PM/AM lifecycle regressions against supplied app classes, including old-head controls."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--group", choices=("all", "services", "pending", "events", "broadcasts", "providers", "activities", "starting", "provider-install", "activity-start", "termination"), default="all")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for recording native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-enabled-lifecycle-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libenabled_lifecycle_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/enabled_lifecycle_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(ROOT / "tests/dex-services/com/gesher/server/PackageEnabledLifecycleFixture.java"),
                        str(ROOT / "tests/package/PackageEnabledLifecycleHost.java")], check=True)
        cleaner = work / "cleaner" / "sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "PackageEnabledLifecycleHost", str(library), args.group]
        for mode in ("0", "1"):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("data-" + mode))
            env["GESHER_MICROG_SPOOF"] = mode; env["GESHER_LOCALES"] = "en-US"
            subprocess.run(command, env=env, check=True, timeout=120)
    if args.group == "all":
        # The pinned framework recipe already executes this candidate-head entry
        # point. Keep authority candidates coupled to enabled/publication coverage.
        authority = [sys.executable, str(ROOT / "tools/test_provider_authority_index.py")]
        for name in ("framework_classes", "core_classes", "app_classes", "apksig", "java_home"):
            authority.extend(("--" + name.replace("_", "-"), str(getattr(args, name).resolve())))
        subprocess.run(authority, check=True, timeout=180)


if __name__ == "__main__":
    main()
