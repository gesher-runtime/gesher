#!/usr/bin/env python3
"""Compare complete PMS UID queries in alternating fresh host JVMs.

Both variants compile their complete PMS/visibility sources against actual built
app/framework/core classes and the same fixture. No production helper is extracted
or reimplemented. Results concern this component on the host JVM, not ART startup
or a complete application. Setup is outside the recorded completed-query interval.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ("app/java/com/gesher/server/PackageManagerService.java",
           "app/java/com/gesher/server/PackageVisibility.java")
FIXTURES = ("tests/dex-services/gesher/services/PackageVisibilityContractTest.java",
            "tests/java-performance/PackageUidLookupWorkHost.java",
            "tests/java-performance/PackageMetadataSizeAgent.java",
            "tests/java-performance/PackageVisibilityStorageHost.java")
DEFAULT_CASES = [
    dict(kind="uid-warm-name", packages=4096, uids=128, iterations=10000),
    dict(kind="uid-warm-owners", packages=4096, uids=128, iterations=5000),
    dict(kind="uid-warm-permission", packages=4096, uids=128, iterations=5000),
    dict(kind="uid-warm-bulk", packages=4096, uids=128, iterations=200),
    dict(kind="uid-cold-bulk", packages=4096, uids=128, iterations=200),
    dict(kind="uid-warm-group", packages=4096, uids=128, iterations=1000),
    dict(kind="uid-cold-name", packages=4096, uids=128, iterations=10000),
    dict(kind="uid-cold-name", packages=4, uids=128, iterations=200000),
    dict(kind="uid-warm-name", packages=4, uids=128, iterations=200000),
    dict(kind="uid-cold-bulk", packages=4, uids=4, iterations=100000),
    dict(kind="uid-cold-bulk", packages=16, uids=16, iterations=30000),
    dict(kind="uid-cold-bulk", packages=32, uids=16, iterations=30000),
    dict(kind="uid-cold-bulk", packages=16, uids=128, iterations=10000),
    dict(kind="uid-cold-repeated", packages=4096, uids=128, iterations=30000),
    dict(kind="uid-cold-core", packages=4096, uids=128, iterations=10000),
    dict(kind="visibility-selective", packages=2, uids=1, iterations=10000),
    dict(kind="visibility-selective", packages=50, uids=1, iterations=10000),
    dict(kind="visibility-hidden", packages=4096, uids=1, iterations=1000),
    dict(kind="visibility-bulk", packages=4096, uids=1, iterations=100),
    dict(kind="visibility-groups", packages=256, uids=1, iterations=1000),
    dict(kind="visibility-cold", packages=2, uids=1, iterations=10000),
    dict(kind="visibility-cold", packages=50, uids=1, iterations=1000),
    dict(kind="visibility-cold", packages=4096, uids=1, iterations=50),
]
FIRST_CASES = [
    dict(kind="uid-cold-name", packages=1, uids=1, iterations=1),
    dict(kind="uid-cold-name", packages=32, uids=1, iterations=1),
    dict(kind="uid-warm-name", packages=4096, uids=1, iterations=1),
    dict(kind="uid-cold-bulk", packages=16, uids=16, iterations=1),
    dict(kind="uid-cold-bulk", packages=32, uids=128, iterations=1),
    dict(kind="uid-cold-bulk", packages=4096, uids=128, iterations=1),
    dict(kind="uid-cold-permission", packages=4096, uids=1, iterations=1),
    dict(kind="visibility-cold", packages=2, uids=1, iterations=1),
    dict(kind="visibility-cold", packages=4096, uids=1, iterations=1),
]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "baseline-app-classes",
                 "candidate-app-classes", "apksig", "work-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--first-rounds", type=int, default=6)
    parser.add_argument("--phase", choices=("warm", "first", "both"), default="both")
    parser.add_argument("--cases", type=Path, help="Explicit JSON case list for a scoped control replay")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    if work.exists() or args.output.exists():
        parser.error("Use fresh work/output paths; prior measurements are never overwritten")
    work.mkdir(parents=True); args.output.parent.mkdir(parents=True, exist_ok=True)
    java, javac = args.java_home / "bin/java", args.java_home / "bin/javac"
    native = work / "libuid_host.so"
    subprocess.run([shutil.which("cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                    "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                    str(ROOT / "tests/location/framework_host.c"), "-o", str(native)], check=True)
    cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
    for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
        shutil.copy2(source, cleaner / source.name)
    record = dict(schema=1, baseline_ref=args.baseline_ref,
                  current_head=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                  java_version=subprocess.run([str(java), "-version"], text=True, capture_output=True,
                                              check=True).stderr,
                  boundary="Complete public PMS calls plus result checks; caller allocation/CPU and process CPU. "
                           "Setup excluded. Fresh JVM per receipt; seven correlated warm samples or one first query. "
                           "Constructed admitted metadata and declared native host boundaries; no ART/Windows claim.",
                  sources={}, dependencies={}, receipts=[], storage=[])
    commands = {}
    for variant in ("baseline", "candidate"):
        classes = work / variant / "classes"; classes.mkdir(parents=True)
        source_paths = []
        record["sources"][variant] = {}
        for relative in (*SOURCES, *FIXTURES):
            data = (subprocess.check_output(["git", "show", args.baseline_ref + ":" + relative], cwd=ROOT)
                    if variant == "baseline" and relative in SOURCES else (ROOT / relative).read_bytes())
            source = work / variant / "source" / relative; source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(data); source_paths.append(str(source))
            record["sources"][variant][relative] = sha(data)
        app = args.baseline_app_classes if variant == "baseline" else args.candidate_app_classes
        classpath = os.pathsep.join(str(p.resolve()) for p in
                                   (app, args.framework_classes, args.core_classes, args.apksig))
        record["dependencies"][variant + "_app_sha256"] = sha(app.read_bytes())
        compile_command = [str(javac), "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", classpath,
                           "-d", str(classes), *source_paths]
        with (work / (variant + "-compile.log")).open("w") as log:
            subprocess.run(compile_command, check=True, stdout=log, stderr=subprocess.STDOUT)
        commands[variant] = [str(java), "-XX:-UsePerfData", "-Xbatch", "-Xmx256m", "--patch-module",
                             "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                             "PackageUidLookupWorkHost", str(native)]
    record["dependencies"]["apksig_sha256"] = sha(args.apksig.read_bytes())
    record["dependencies"]["framework_classes"] = str(args.framework_classes.resolve())
    record["dependencies"]["core_classes"] = str(args.core_classes.resolve())
    record["native_boundaries"] = {str(p.relative_to(ROOT)): sha(p.read_bytes()) for p in
            [ROOT / ("tests/" + name + "/framework_host.c") for name in
             ("location", "component-caller", "uri", "package", "notification")]}
    manifest = work / "size-agent-manifest.mf"
    manifest.write_text("Premain-Class: PackageMetadataSizeAgent\n")
    agent = work / "size-agent.jar"
    subprocess.run([str(args.java_home / "bin/jar"), "cfm", str(agent), str(manifest),
                    "-C", str(work / "candidate/classes"), "PackageMetadataSizeAgent.class"], check=True)

    def save() -> None:
        temporary = args.output.with_suffix(args.output.suffix + ".tmp")
        temporary.write_text(json.dumps(record, indent=2) + "\n")
        temporary.replace(args.output)

    save()
    for variant in ("baseline", "candidate"):
        for count, grouped in ((1, False), (4, False), (16, False), (32, False),
                               (4096, False), (256, True), (4096, True)):
            command = commands[variant].copy()
            command[1:1] = ["-javaagent:" + str(agent), "--add-opens=java.base/java.util=ALL-UNNAMED"]
            command[command.index("PackageUidLookupWorkHost")] = "PackageVisibilityStorageHost"
            command += [str(count), str(grouped).lower()]
            environment = os.environ.copy(); environment["GESHER_MICROG_SPOOF"] = "1"
            environment["ANDROID_DATA"] = str(work / ("storage-" + variant + "-" + str(count) + str(grouped)))
            environment.pop("GESHER_BINDER_UID", None)
            result = subprocess.run(command, text=True, capture_output=True, env=environment, check=True, timeout=60)
            output = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{"packages":')]
            if len(output) != 1: raise RuntimeError("Missing structural storage receipt")
            record["storage"].append(dict(variant=variant, measurement=output[0], command=command))
            save()
    for phase in ("warm", "first"):
        if args.phase not in (phase, "both"):
            continue
        cases = json.loads(args.cases.read_text()) if args.cases else (FIRST_CASES if phase == "first" else DEFAULT_CASES)
        rounds = args.first_rounds if phase == "first" else args.rounds
        for case_index, case in enumerate(cases):
            if phase == "first" and case["iterations"] != 1:
                parser.error("First-query cases must use iterations=1")
            for round_index in range(rounds):
                for variant in ("baseline", "candidate", "candidate", "baseline"):
                    sequence = len(record["receipts"])
                    command = commands[variant] + [str(case["packages"]), case["kind"],
                               str(case["iterations"]), str(case["uids"])] + (["--first"] if phase == "first" else [])
                    environment = os.environ.copy()
                    environment["ANDROID_DATA"] = str(work / ("data-" + str(sequence)))
                    environment["GESHER_MICROG_SPOOF"] = "1"; environment.pop("GESHER_BINDER_UID", None)
                    started = time.monotonic_ns()
                    result = subprocess.run(command, env=environment, text=True, capture_output=True, timeout=180)
                    elapsed = time.monotonic_ns() - started
                    log = work / ("sample-%04d.log" % sequence)
                    log.write_text(result.stdout + result.stderr)
                    if result.returncode:
                        raise RuntimeError(f"{variant} {case} failed; retained {log}")
                    outputs = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{"kind":')]
                    if len(outputs) != 1 or outputs[0]["kind"] != case["kind"]:
                        raise RuntimeError("Missing or mismatched actual workload receipt: " + str(log))
                    record["receipts"].append(dict(sequence=sequence, phase=phase, case=case_index,
                            round=round_index, variant=variant, command=command, process_elapsed_ns=elapsed,
                            measurement=outputs[0], log_sha256=sha(log.read_bytes())))
                    save()
            print(json.dumps(dict(completed=case, phase=phase, receipts=len(record["receipts"]))), flush=True)
    print("Saved " + str(args.output), flush=True)


if __name__ == "__main__":
    main()
