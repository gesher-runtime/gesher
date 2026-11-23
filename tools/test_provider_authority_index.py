#!/usr/bin/env python3
"""Run production provider resolution and Android metadata, with host JNI boundaries."""
from __future__ import annotations
import argparse
import hashlib
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
    parser.add_argument("--source-ref", help="Compile prior PMS/provider sources for the same benchmark")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the existing host JNI boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-provider-authority-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libprovider_authority_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("PackageManagerService", "ContentProviders", "ApkPackage"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + ".java")
                result = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                        cwd=ROOT, check=True, capture_output=True)
                source.write_bytes(result.stdout)
            print(name + " SHA-256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/ProviderAuthorityIndexTest.java"),
                    str(ROOT / "tests/dex-services/gesher/services/ProviderInitOrderTest.java"),
                    str(ROOT / "tests/package/ProviderAuthorityHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        command = [str(args.java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                   "ProviderAuthorityHost", str(library)]
        if args.source_ref:
            command.append("baseline")
        subprocess.run(command, env=environment, check=True, timeout=120)


if __name__ == "__main__":
    main()
