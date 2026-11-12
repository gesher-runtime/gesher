#!/usr/bin/env python3
"""Exercise production domain-verification service through the real Android manager.

The host uses compiled framework/core/app inputs and existing native host
boundaries. The same Java fixture runs through generated Binder parcels on Windows.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--apksig-jar", type=Path)
    parser.add_argument("--source-ref", help="Run the same fixture against a prior service revision")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if cc is None:
        parser.error("A C compiler is required for native host boundaries")
    paths = [args.app_classes, args.framework_classes, args.core_classes]
    if args.apksig_jar:
        paths.append(args.apksig_jar)
    classpath = os.pathsep.join(str(path.resolve()) for path in paths)
    with tempfile.TemporaryDirectory(prefix="gesher-domain-verification-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libdomain_verification_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"),
                        "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/framework_host.c"), "-o", str(library)],
                       check=True, timeout=60)
        relative = "app/java/com/gesher/server/DeviceServices.java"
        service = ROOT / relative
        if args.source_ref:
            service = work / "DeviceServices.java"
            old = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                 cwd=ROOT, check=True, stdout=subprocess.PIPE)
            service.write_bytes(old.stdout)
        sources = [service, ROOT / "tests/dex-services/gesher/services/DomainVerificationContractTest.java",
                   ROOT / "tests/domain-verification/DomainVerificationHost.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), *map(str, sources)], check=True, timeout=90)
        env = os.environ.copy()
        env["ANDROID_DATA"] = str(work / "data")
        subprocess.run([str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "DomainVerificationHost", str(library)], env=env, check=True, timeout=45)


if __name__ == "__main__":
    main()
