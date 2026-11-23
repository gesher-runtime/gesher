#!/usr/bin/env python3
"""Run production clipboard permission/focus/callback contracts with the declared host JNI boundary.

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
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        parser.error("A C compiler is required for the host native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-clipboard-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libclipboard_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/clipboard/framework_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(ROOT / "tests/dex-services/gesher/services/ClipboardFixture.java"),
                        str(ROOT / "tests/dex-services/gesher/services/ClipboardSecurityContractTest.java"),
                        str(ROOT / "tests/dex-services/gesher/services/ClipboardCallbackTest.java"),
                        str(ROOT / "tests/clipboard/ClipboardHost.java")], check=True)
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy()
        env.pop("GESHER_BINDER_UID", None)
        for mode in ("0", "1"):
            env["ANDROID_DATA"] = str(work / ("data-" + mode))
            env["GESHER_MICROG_SPOOF"] = mode
            subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                            "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                            "ClipboardHost", str(library)], env=env, check=True, timeout=75)


if __name__ == "__main__":
    main()
