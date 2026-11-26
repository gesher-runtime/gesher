#!/usr/bin/env python3
"""Run production PMS intent resolution with real Android matching/metadata and host JNI boundaries."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_intent_candidates(*, java_home: Path, framework_classes: Path, core_classes: Path,
                          app_classes: Path, apksig: Path, source_ref: str | None = None,
                          metadata_case: str | None = None, metadata_scenario: str = "activity-broad",
                          metadata_components: int = 128, metadata_queries: int = 100,
                          metadata_samples: int = 7, compile_output: Path | None = None) -> None:
    cc = shutil.which("cc")
    if not cc:
        raise RuntimeError("A C compiler is required for the existing host JNI boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (app_classes, framework_classes, core_classes, apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-intent-candidates-") as directory:
        work = compile_output or Path(directory)
        if compile_output:
            work.mkdir(parents=True, exist_ok=True)
            if any(work.iterdir()):
                raise ValueError("--compile-output must be empty; do not mix source revisions")
        classes = work / "classes"; classes.mkdir()
        library = work / "libintent_candidates_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(java_home / "include"), "-I" + str(java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        source_hashes = {}
        for name in ("PackageManagerService", "ApkPackage"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.run(["git", "show", source_ref + ":" + relative],
                                                 cwd=ROOT, check=True, capture_output=True).stdout)
            source_hashes[relative] = hashlib.sha256(source.read_bytes()).hexdigest()
            print(name + " SHA-256 " + source_hashes[relative], flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/IntentCandidateIndexTest.java"),
                    str(ROOT / "tests/package/IntentCandidateHost.java")]
        for source in sources[2:]:
            source_hashes[str(Path(source).relative_to(ROOT))] = hashlib.sha256(Path(source).read_bytes()).hexdigest()
        subprocess.run([str(java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        if compile_output:
            (work / "compiled-sources.json").write_text(json.dumps({"source_ref": source_ref,
                    "sha256": source_hashes, "java_home": str(java_home.resolve()),
                    "classpath": classpath}, indent=2) + "\n")
            print("Compiled actual production and fixture classes at " + str(work.resolve()), flush=True)
            return
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        command = [str(java_home / "bin/java"), "-XX:-UsePerfData", "-cp", str(classes) + os.pathsep + classpath,
                   "IntentCandidateHost", str(library)]
        if metadata_case:
            command += ["metadata-" + metadata_case, str(metadata_components), str(metadata_queries),
                        str(metadata_samples), metadata_scenario]
        elif source_ref:
            command.append("baseline")
        subprocess.run(command, env=environment, check=True, timeout=120)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile prior production sources for the same contracts and workload")
    parser.add_argument("--compile-output", type=Path,
                        help="Compile into an empty directory for repeated fresh-JVM workload measurements")
    parser.add_argument("--metadata-case", choices=("contracts", "work", "coherence", "benchmark"),
                        help="Run component metadata contracts, strict work budget, or uninstrumented measurements")
    parser.add_argument("--metadata-scenario", choices=("activity-broad", "service-broad", "receiver-broad",
                        "activity-sparse", "activity-single", "activity-miss", "activity-disabled"), default="activity-broad")
    parser.add_argument("--metadata-components", type=int, default=128)
    parser.add_argument("--metadata-queries", type=int, default=100)
    parser.add_argument("--metadata-samples", type=int, default=7)
    args = parser.parse_args()
    run_intent_candidates(**vars(args))


if __name__ == "__main__":
    main()
