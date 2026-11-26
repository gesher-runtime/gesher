#!/usr/bin/env python3
"""Exercise actual foreground-service promotion, with an optional pre-fix regression control."""
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
    parser.add_argument("--source-ref", help="Compile a previous ActiveServices as the negative control")
    parser.add_argument("--scenario", choices=("all", "base", "types", "permissions", "normalization", "shared-uid"), default="all")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if compiler is None:
        parser.error("A C compiler is required for the host initialization boundary")
    classpath = os.pathsep.join(str(p.resolve()) for p in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-fgs-admission-") as directory:
        work = Path(directory)
        classes = work / "classes"; classes.mkdir()
        library = work / "libfgs_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/ActiveServices.java"
        production = ROOT / relative
        if args.source_ref:
            production = work / "ActiveServices.java"
            production.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                    cwd=ROOT, capture_output=True, check=True).stdout)
        sources = [production, ROOT / "tests/dex-services/gesher/services/FixtureServiceContext.java",
                   ROOT / "tests/dex-services/gesher/services/ForegroundServiceAdmissionTest.java",
                   ROOT / "tests/activity-finish/ForegroundServiceAdmissionHost.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *(str(p) for p in sources)], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for entry in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(entry, cleaner / entry.name)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_MICROG_SPOOF"] = "1"
        env.pop("GESHER_BINDER_UID", None)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "ForegroundServiceAdmissionHost", str(library), args.scenario],
                       env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
