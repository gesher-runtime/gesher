#!/usr/bin/env python3
"""Run production receiver-registration policy with explicit host initialization boundaries.

The Windows track separately runs generated Binder Parcel dispatch. This host run
uses real framework/service Java but does not execute ART or application APKs.
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
    parser.add_argument("--source-ref", help="Compile the controller at this ref as a negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the host native boundaries")
    cp = os.pathsep.join(str(p.resolve()) for p in
                        (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-receiver-export-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libreceiver_export_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/pending-intent/framework_host.c"), "-o", str(library)], check=True)
        sources = [ROOT / "tests/dex-services/gesher/services/ReceiverExportContractTest.java",
                   ROOT / "tests/receiver-exports/ReceiverExportsHost.java"]
        if args.source_ref:
            source = work / "BroadcastController.java"
            source.write_bytes(subprocess.check_output(["git", "show", args.source_ref
                    + ":app/java/com/gesher/server/BroadcastController.java"], cwd=ROOT))
            sources.append(source)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", cp,
                        "-d", str(classes), *map(str, sources)], check=True)
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy()
        env["ANDROID_DATA"] = str(work / "data")
        env.pop("GESHER_BINDER_UID", None)
        for mode in ("0", "1"):
            env["GESHER_MICROG_SPOOF"] = mode
            print("Receiver export contracts, compatibility mode=" + mode, flush=True)
            subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                            "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + cp,
                            "ReceiverExportsHost", str(library)], env=env, check=True, timeout=75)


if __name__ == "__main__":
    main()
