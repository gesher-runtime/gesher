#!/usr/bin/env python3
"""Exercise production BlobStore admission/accounting with bounded seeded metadata.

The Java fixture calls the real service and asynchronous commit worker, persists
real index files and preserves earlier contributions on rejection. Metadata is
seeded to reach quota limits without allocating gigabytes or creating 1,000
sessions. No descriptor revocation, Windows/ART or remote Binder result is claimed.
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
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile an older BlobStoreService as a negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the declared native host boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-blob-quota-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libblob_quota_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/settings/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/BlobStoreService.java"
        service = ROOT / relative
        if args.source_ref:
            service = work / "BlobStoreService.java"
            service.write_bytes(subprocess.check_output(
                    ["git", "show", args.source_ref + ":" + relative], cwd=ROOT))
        sources = [service, ROOT / "app/java/com/gesher/server/BlobStoreFiles.java",
                   ROOT / "app/java/com/gesher/server/RevocableFileDescriptors.java",
                   ROOT / "tests/dex-services/gesher/services/BlobStoreQuotaContractTest.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), *map(str, sources)], check=True)
        cleaner = work / "cleaner" / "sun" / "misc"
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "gesher.services.BlobStoreQuotaContractTest", str(library)],
                       env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
