#!/usr/bin/env python3
"""Run actual PMS/static visibility/APM Java with constructed manifests and host JNI boundaries."""
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
    parser.add_argument("--source-ref", help="Use a prior PMS as a public-wrapper negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-package-visibility-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libpackage_visibility_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/PackageManagerService.java"
        service = ROOT / relative
        if args.source_ref:
            service = work / "PackageManagerService.java"
            old = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                 cwd=ROOT, check=True, capture_output=True)
            service.write_bytes(old.stdout)
        sources = [str(service), str(ROOT / "app/java/com/gesher/server/PackageVisibility.java"),
                   str(ROOT / "tests/dex-services/gesher/services/PackageVisibilityContractTest.java"),
                   str(ROOT / "tests/package/PackageVisibilityHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes), *sources], check=True)
        env = os.environ.copy()
        command = [str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                   "-cp", str(classes) + os.pathsep + classpath, "PackageVisibilityHost", str(library)]
        for mode in ("1", "0"):
            env["ANDROID_DATA"] = str(work / ("data-" + mode)); env["GESHER_MICROG_SPOOF"] = mode
            subprocess.run(command, env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
