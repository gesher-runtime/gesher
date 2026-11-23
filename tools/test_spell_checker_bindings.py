#!/usr/bin/env python3
"""Exercise production spell-checker provider groups and actual ActiveServices ownership."""
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
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref", help="Compile historical session/catalog sources for actual baseline work")
    parser.add_argument("--require-shared", action="store_true", help="Apply the candidate connection bound to a historical negative control")
    parser.add_argument("--case", choices=("all", "contracts", "work"), default="all")
    args = parser.parse_args()
    if args.source_ref and args.case != "work":
        parser.error("The historical implementation supports the work comparison, not new group contract gates")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-spell-bindings-") as temporary:
        work = Path(temporary); classes = work / "classes"; classes.mkdir()
        library = work / "libspell_bindings.so"
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("SpellCheckerCatalog", "SpellCheckerSessions"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT))
            print(name + " SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(source)
        sources += [ROOT / "tests/dex-services/gesher/services/SpellCheckerBindingContractTest.java",
                    ROOT / "tests/spell-checker/SpellCheckerBindingsHost.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *map(str, sources)], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data"); env["GESHER_MICROG_SPOOF"] = "1"
        env["GESHER_LOCALES"] = "en-US"
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                        "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                        "SpellCheckerBindingsHost", str(library), args.case,
                        str(bool(args.source_ref) and not args.require_shared).lower()],
                       env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
