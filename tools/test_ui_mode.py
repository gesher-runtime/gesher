#!/usr/bin/env python3
"""Run production UiMode/ATM configuration contracts with the declared host JNI boundary.

Pass freshly built app classes. This does not replace the Windows/ART Binder
fixture in 94-framework-fixes.ps1 or claim native application execution.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--microg-spoof", choices=("0", "1"), default="1")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        parser.error("A C compiler is required for the host native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-ui-mode-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libui-mode_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/ui-mode/framework_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(ROOT / "tests/dex-services/gesher/services/UiModeContractTest.java"),
                        str(ROOT / "tests/ui-mode/UiModeHost.java"),
                        str(ROOT / "tests/dex-services/com/gesher/server/UiModeFixture.java"),
                        str(ROOT / "tests/ui-mode/SolarComparisonHost.java"),
                        str(ROOT / "tests/ui-mode/aosp/com/ibm/icu/impl/CalendarAstronomer.java")], check=True)
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy()
        env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_MICROG_SPOOF"] = args.microg_spoof
        env.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-cp",
                        str(classes) + os.pathsep + classpath, "com.gesher.server.SolarComparisonHost"],
                       check=True, timeout=30)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                        "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                        "UiModeHost", str(library)], env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
