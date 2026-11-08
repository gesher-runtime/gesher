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
            if name == "ActivityClientController" and args.controller_source_ref:
                source = work / (name + ".java")
                old = subprocess.run(["git", "show", args.controller_source_ref + ":" + relative],
                                     cwd=ROOT, check=True, capture_output=True)
                source.write_bytes(old.stdout)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/com/gesher/server/ComponentCallerFixture.java"),
                    str(ROOT / "tests/component-caller/ComponentCallerHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes), *sources], check=True)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data"); env["GESHER_MICROG_SPOOF"] = "1"
        subprocess.run([str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                        "-cp", str(classes) + os.pathsep + classpath, "ComponentCallerHost", str(library)],
                        env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
