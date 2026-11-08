#!/usr/bin/env python3
"""Exercise actual explicit URI grants, parsed provider metadata and PermissionPolicy on a JVM.

Only native initialization and caller PID/UID are host adapters. The separate
UriGrantContractTest main uses the actual AIDL/Parcel path on Windows ART.
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
    parser.add_argument("--source-ref", help="Compile prior URI/provider sources as a negative control")
    parser.add_argument("--case", choices=("delegation", "identity", "modes", "patterns", "revocation", "retirement", "validation"))
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-uri-grants-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        library = work / "liburi_grants_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("UriGrants", "ContentProviders"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + ".java")
                result = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                        cwd=ROOT, check=True, capture_output=True)
                source.write_bytes(result.stdout)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/UriGrantContractTest.java"),
                    str(ROOT / "tests/uri/UriGrantHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "1"
        command = [str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                   "-cp", str(classes) + os.pathsep + classpath, "UriGrantHost", str(library)]
        if args.case:
            command.append(args.case)
        subprocess.run(command, env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
