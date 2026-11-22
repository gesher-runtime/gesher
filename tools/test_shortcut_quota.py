#!/usr/bin/env python3
"""Actual shortcut quota, identity and empty-snapshot disk publication on the host JVM."""
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
    for name in ("framework-classes", "core-classes", "app-classes", "java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    apksig = os.environ.get("APKSIG_JAR")
    if not apksig and os.environ.get("RUNNER_TEMP"):
        apksig = str(Path(os.environ["RUNNER_TEMP"]) / "gesher-apksig.jar")
    parser.add_argument("--apksig-jar", type=Path, default=Path(apksig) if apksig else None)
    parser.add_argument("--case", choices=("all", "contracts", "work", "graph"), default="all")
    parser.add_argument("--graph-nodes", type=int, default=64)
    parser.add_argument("--graph-queries", type=int, default=256)
    parser.add_argument("--expected-published", type=int, default=10)
    parser.add_argument("--source-ref")
    parser.add_argument("--file-events", type=Path, help="Retain actual JVM FileWrite events from the production workload")
    args = parser.parse_args()
    if not args.apksig_jar or not args.apksig_jar.is_file():
        parser.error("--apksig-jar or the framework job's APKSIG_JAR/RUNNER_TEMP input is required")
    classpath = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    classpath += os.pathsep + str(args.apksig_jar.resolve())
    with tempfile.TemporaryDirectory(prefix="gesher-shortcut-quota-") as temporary:
        work = Path(temporary); classes = work / "classes"; classes.mkdir()
        library = work / "libshortcut_quota.so"
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/location/framework_host.c"),
                        str(ROOT / "tests/zygote/path_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("DesktopShortcutService", "ShortcutQuota", "ShortcutProcessState", "PolicyStateWriter", "CallerIdentity",
                     "ActivityManagerService", "ActivityTaskManagerService", "ActiveServices", "ContentProviders", "PowerManagerService"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                if name in ("ShortcutQuota", "ShortcutProcessState"):
                    continue
                source = work / (name + ".java")
                source.write_bytes(subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT))
            print(name + " SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/shortcuts/ShortcutQuotaHost.java"),
                    str(ROOT / "tests/dex-services/gesher/services/ShortcutQuotaContractTest.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        for path in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(path, cleaner / path.name)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_SHORTCUTS"] = "0"; env["GESHER_MICROG_SPOOF"] = "1"; env["GESHER_LOCALES"] = "en-US"
        env.pop("GESHER_BINDER_UID", None)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "ShortcutQuotaHost", str(library), args.case, str(args.expected_published),
                   str(args.file_events.resolve() if args.file_events else work / "writes.jfr"),
                   str(args.graph_nodes), str(args.graph_queries)]
        subprocess.run(command, env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
