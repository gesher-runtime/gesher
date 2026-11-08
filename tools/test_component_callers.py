#!/usr/bin/env python3
"""Run actual ComponentCaller/controller/URI policy with constructed parsed manifests on a JVM."""
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
    parser.add_argument("--controller-source-ref", help="Use a prior controller as a negative control")
    parser.add_argument("--atm-source-ref", help="Use a prior task manager as an enrollment negative control")
    parser.add_argument("--fixture-source-ref", help="Use a prior fixture as a runtime-package negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-component-callers-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libcomponent_caller_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/component-caller/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("ActivityCallerState", "ActivityClientController", "ActivityTaskManagerService",
                     "ActivityManagerService", "PendingIntents", "UriGrants"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            source_ref = (args.controller_source_ref if name == "ActivityClientController" else
                    args.atm_source_ref if name == "ActivityTaskManagerService" else None)
            if source_ref:
                source = work / (name + ".java")
                old = subprocess.run(["git", "show", source_ref + ":" + relative],
                                     cwd=ROOT, check=True, capture_output=True)
                source.write_bytes(old.stdout)
            sources.append(str(source))
        fixture_relative = "tests/dex-services/com/gesher/server/ComponentCallerFixture.java"
        fixture = ROOT / fixture_relative
        if args.fixture_source_ref:
            fixture = work / "ComponentCallerFixture.java"
            old = subprocess.run(["git", "show", args.fixture_source_ref + ":" + fixture_relative],
                                 cwd=ROOT, check=True, capture_output=True)
            fixture.write_bytes(old.stdout)
        sources += [str(fixture), str(ROOT / "tests/component-caller/ComponentCallerHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes), *sources], check=True)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data")
        command = [str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                   "-cp", str(classes) + os.pathsep + classpath, "ComponentCallerHost", str(library)]
        # Use separate real environments; do not substitute the production mode
        # predicate or mutate HotSpot's cached System.getenv result in-process.
        for bootstrap_only in (False, True):
            env["GESHER_MICROG_SPOOF"] = "0" if bootstrap_only else "1"
            subprocess.run(command + (["bootstrap-only"] if bootstrap_only else []),
                           env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
