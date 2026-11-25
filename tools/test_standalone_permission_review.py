#!/usr/bin/env python3
"""Exercise production standalone admission before APK code, with explicit host boundaries.

--source-ref compiles that exact InProcessApplication as a regression control;
the other permission/runtime classes are always compiled from this checkout.
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
    parser.add_argument("--source-ref")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        parser.error("A C compiler is required for the declared host native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-standalone-review-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libstandalone_review_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/standalone-review/framework_host.c"), "-o", str(library)], check=True)
        admission = ROOT / "app/java/com/gesher/server/InProcessApplication.java"
        if args.source_ref:
            relative = admission.relative_to(ROOT).as_posix()
            admission = work / "InProcessApplication.java"
            admission.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                    cwd=ROOT, capture_output=True, check=True).stdout)
        sources = [admission, ROOT / "tests/standalone-review/StandaloneReviewHost.java"]
        sources += [ROOT / ("app/java/com/gesher/server/" + name + ".java") for name in
                    ("PermissionRequests", "PermissionPolicy", "ActivityTaskManagerService", "ActivityManagerService")]
        sources += [ROOT / "app/java/com/gesher/app/locale/UiMessages.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *(str(source) for source in sources)], check=True)
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy()
        env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_MICROG_SPOOF"] = "1"
        env["GESHER_LOCALES"] = "en-US"
        env.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                        "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                        "com.gesher.server.StandaloneReviewHost", str(library)], env=env, check=True, timeout=75)


if __name__ == "__main__":
    main()
