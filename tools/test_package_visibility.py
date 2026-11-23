#!/usr/bin/env python3
"""Run actual PMS/static visibility/APM Java with constructed manifests and host JNI boundaries."""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_intent_candidate_index import run_intent_candidates

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile prior complete PMS and visibility sources")
    parser.add_argument("--work", choices=("selective", "hidden", "bulk", "groups", "cold"),
                        help="Measure complete public queries; no metadata counters are used")
    parser.add_argument("--work-packages", type=int, default=4096)
    parser.add_argument("--work-iterations", type=int, default=1000)
    parser.add_argument("--work-check", action="store_true", help="Enforce the production metadata-read budget")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for native host boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-package-visibility-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libpackage_visibility_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("PackageManagerService", "PackageVisibility"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                                  cwd=ROOT, check=True, capture_output=True).stdout)
            print(name + " SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/PackageVisibilityContractTest.java"),
                    str(ROOT / "tests/package/PackageVisibilityHost.java"),
                    str(ROOT / "tests/java-performance/PackageVisibilityWorkHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes), *sources], check=True)
        env = os.environ.copy()
        if args.work or args.work_check:
            env["ANDROID_DATA"] = str(work / "data-work"); env["GESHER_MICROG_SPOOF"] = "1"
            workload = (["--verify-work"] if args.work_check else
                        [str(args.work_packages), args.work, str(args.work_iterations)])
            subprocess.run([str(args.java_home / "bin/java"), "-Xbatch", "-Xmx256m",
                            "-Djava.io.tmpdir=" + str(work), "-cp", str(classes) + os.pathsep + classpath,
                            "PackageVisibilityWorkHost", str(library), *workload],
                           env=env, check=True, timeout=180)
            return
        command = [str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                   "-cp", str(classes) + os.pathsep + classpath, "PackageVisibilityHost", str(library)]
        for mode in ("1", "0"):
            env["ANDROID_DATA"] = str(work / ("data-" + mode)); env["GESHER_MICROG_SPOOF"] = mode
            subprocess.run(command, env=env, check=True, timeout=90)

    # The immutable framework recipe already executes this candidate-head entry
    # point with the real framework/core/app dependencies.
    if args.source_ref is None:
        run_intent_candidates(java_home=args.java_home, framework_classes=args.framework_classes,
                              core_classes=args.core_classes, app_classes=args.app_classes, apksig=args.apksig)


if __name__ == "__main__":
    main()
