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
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--source-ref", help="Compile prior complete PMS and visibility sources")
    source.add_argument("--source-dir", type=Path, help="Compile complete source files from an explicit negative-control tree")
    uid_kinds = tuple("uid-" + state + "-" + kind for state in ("cold", "warm")
                      for kind in ("name", "owners", "permission", "bulk", "core", "repeated", "group"))
    parser.add_argument("--work", choices=("selective", "hidden", "bulk", "groups", "cold", *uid_kinds),
                        help="Measure complete public queries; no metadata counters are used")
    parser.add_argument("--work-packages", type=int, default=4096)
    parser.add_argument("--work-iterations", type=int, default=1000)
    parser.add_argument("--work-check", action="store_true", help="Enforce the production metadata-read budget")
    parser.add_argument("--uid-work-check", action="store_true", help="Enforce the actual UID registry-iteration budgets")
    parser.add_argument("--uid-contract-check", action="store_true", help="Run UID API parity/lifetime controls without optimization budgets")
    parser.add_argument("--uid-count", type=int, default=128)
    parser.add_argument("--work-first", action="store_true", help="One completed first-query measurement, without warm-up")
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
                        str(ROOT / "tests/location/framework_host.c"), "-o", str(library)], check=True)
        # Reuse the UID-policy fixture's Binder-holder/clock boundary for live
        # permission grants; its included URI identity adapter stays unchanged.
        cleaner = work / "cleaner" / "sun" / "misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        sources = []
        for name in ("PackageManagerService", "PackageVisibility"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if args.source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                                  cwd=ROOT, check=True, capture_output=True).stdout)
            elif args.source_dir:
                source = args.source_dir / relative
            print(name + " SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/PackageVisibilityContractTest.java"),
                    str(ROOT / "tests/package/PackageVisibilityHost.java"),
                    str(ROOT / "tests/java-performance/PackageVisibilityWorkHost.java"),
                    str(ROOT / "tests/java-performance/PackageUidLookupWorkHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes), *sources], check=True)
        env = os.environ.copy()
        if args.work or args.work_check or args.uid_work_check:
            env["ANDROID_DATA"] = str(work / "data-work"); env["GESHER_MICROG_SPOOF"] = "1"
            uid = args.uid_work_check or bool(args.work and args.work.startswith("uid-"))
            entry = "PackageUidLookupWorkHost" if uid else "PackageVisibilityWorkHost"
            workload = (["--verify-work"] if args.work_check or args.uid_work_check else
                        [str(args.work_packages), args.work, str(args.work_iterations)])
            if uid and not args.uid_work_check:
                workload += [str(args.uid_count)] + (["--first"] if args.work_first else [])
            subprocess.run([str(args.java_home / "bin/java"), "-Xbatch", "-Xmx256m",
                            "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                            "-Djava.io.tmpdir=" + str(work), "-cp", str(classes) + os.pathsep + classpath,
                            entry, str(library), *workload],
                           env=env, check=True, timeout=180)
            return
        command = [str(args.java_home / "bin/java"), "-Djava.io.tmpdir=" + str(work),
                   "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                   "-cp", str(classes) + os.pathsep + classpath, "PackageVisibilityHost", str(library)]
        if args.uid_contract_check: command.append("--uid-semantics")
        for mode in ("1", "0"):
            env["ANDROID_DATA"] = str(work / ("data-" + mode)); env["GESHER_MICROG_SPOOF"] = mode
            subprocess.run(command, env=env, check=True, timeout=90)

    # The immutable framework recipe already executes this candidate-head entry
    # point with the real framework/core/app dependencies.
    if args.source_ref is None and args.source_dir is None and not args.uid_contract_check:
        run_intent_candidates(java_home=args.java_home, framework_classes=args.framework_classes,
                              core_classes=args.core_classes, app_classes=args.app_classes, apksig=args.apksig)


if __name__ == "__main__":
    main()
