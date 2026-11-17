#!/usr/bin/env python3
"""Exercise unchanged public storage fixtures with real built Android and Gesher Java classes."""
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
    parser.add_argument("--expect-baseline-failure", action="store_true")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the existing host JNI boundary")
    classpath = os.pathsep.join(str(p.resolve()) for p in (
        args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-public-storage-") as directory:
        work = Path(directory)
        library = work / "libstorage_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"),
                        "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/package/framework_host.c"), "-o", str(library)], check=True)
        sources = [ROOT / "tests/package/AppStoragePublicHost.java"]
        if not args.expect_baseline_failure:
            sources += [ROOT / "tests/package/AppStoragePolicyHost.java",
                        ROOT / "tests/dex-services/gesher/services/PackageManifestFlagsTest.java",
                        ROOT / "tests/dex-services/gesher/services/SplitPackageContractTest.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(work), *map(str, sources)], check=True)
        result = subprocess.run([str(args.java_home / "bin/java"), "-cp", str(work) + os.pathsep + classpath,
                                 "AppStoragePublicHost", str(library),
                                 str(ROOT / "tests/dex-services/fixtures/v2only.apk")],
                                text=True, capture_output=True, timeout=90)
        print(result.stdout, end="")
        print(result.stderr, end="")
        if args.expect_baseline_failure:
            expected = "REGRESSION: public ContextImpl credential/device storage aliases the same directory"
            if result.returncode == 0 or expected not in result.stderr:
                raise SystemExit("Baseline did not fail at the specific public storage regression")
            print("Baseline control detected the original public CE/DE alias")
        else:
            result.check_returncode()
            subprocess.run([str(args.java_home / "bin/java"), "-cp", str(work) + os.pathsep + classpath,
                            "com.gesher.server.AppStoragePolicyHost", str(library),
                            str(ROOT / "tests/dex-services/fixtures")], check=True, timeout=90)


if __name__ == "__main__":
    main()
