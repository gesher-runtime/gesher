#!/usr/bin/env python3
"""Run pinned AOSP mode/owner comparisons and the production URI persistence fixture.

The host adapter covers native initialization and Binder caller identity only.
The Windows track also runs the service fixture with native Binder tokens.
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
    parser.add_argument("--oracle-only", action="store_true")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the explicit native host boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in (
        args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-persisted-uris-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "liburi_host.so"
        subprocess.run([
            cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
            "-I" + str(args.java_home / "include"),
            "-I" + str(args.java_home / "include/linux"),
            str(ROOT / "tests/uri/intent_framework_host.c"), "-o", str(library),
        ], check=True)
        sources = [ROOT / "app/java/com/gesher/server/UriPermissionModes.java",
                   ROOT / "tests/uri/UriPermissionModesOracleTest.java"]
        sources += sorted((ROOT / "tests/uri/aosp").glob("*.java"))
        if not args.oracle_only:
            sources += [ROOT / "app/java/com/gesher/server" / (name + ".java")
                        for name in ("UriGrants", "UriGrantsManagerService", "ContentProviders")]
            sources += [ROOT / "tests/dex-services/gesher/services/PersistableUriGrantContractTest.java",
                        ROOT / "tests/uri/PersistableUriGrantHost.java"]
        subprocess.run([
            str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-source", "17", "-target", "17",
            "-cp", classpath, "-d", str(classes), *map(str, sources),
        ], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "1"
        # Current package lifecycle callbacks initialize actual local Binder objects.
        # Reuse the existing owned holder/free adapter and pinned core Cleaner.
        cleaner = work / "cleaner" / "sun/misc"
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-Djava.io.tmpdir=" + str(work),
                   "-cp", str(classes) + os.pathsep + classpath]
        subprocess.run(command + ["com.gesher.server.UriPermissionModesOracleTest", str(library)],
                       env=environment, check=True, timeout=90)
        if not args.oracle_only:
            subprocess.run(command + ["PersistableUriGrantHost", str(library)],
                           env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
