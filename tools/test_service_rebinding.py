#!/usr/bin/env python3
"""Run actual service rebinding and cached LoadedApk Binder delivery on a JVM."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile prior ActiveServices as a regression control")
    parser.add_argument("--scenario", default="all", choices=("all", "retained", "declined", "delayed", "null", "intents", "reentrant", "ordered", "ordered-destroyed", "replacement", "failed-local", "interleaved", "client-death", "force-stop", "retired", "destroyed", "restarted"))
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for native initialization boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-service-rebinding-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libservice_rebinding_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/ActiveServices.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "ActiveServices.java"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                    cwd=ROOT, check=True, capture_output=True).stdout)
        sources = [source, ROOT / "tests/dex-services/gesher/services/ServiceRebindContractTest.java",
                   ROOT / "tests/activity-finish/ServiceRebindHost.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *(str(path) for path in sources)], check=True)
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        cleaner_inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not cleaner_inputs:
            raise ValueError("Android core compile input has no Cleaner classes")
        for entry in cleaner_inputs:
            shutil.copy2(entry, cleaner / entry.name)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_MICROG_SPOOF"] = "1"
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "ServiceRebindHost", str(library), args.scenario], env=env, check=True, timeout=90)
    if args.scenario == "all" and not args.source_ref:
        subprocess.run([sys.executable, str(ROOT / "tools/test_spell_checker_bindings.py"),
                        "--java-home", str(args.java_home), "--framework-classes", str(args.framework_classes),
                        "--core-classes", str(args.core_classes), "--app-classes", str(args.app_classes),
                        "--apksig", str(args.apksig)], check=True)


if __name__ == "__main__":
    main()
