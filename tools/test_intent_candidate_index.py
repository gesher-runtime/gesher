#!/usr/bin/env python3
"""Run production PMS intent resolution with real Android matching/metadata and host JNI boundaries."""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_intent_candidates(*, java_home: Path, framework_classes: Path, core_classes: Path,
                          app_classes: Path, apksig: Path, source_ref: str | None = None) -> None:
    cc = shutil.which("cc")
    if not cc:
        raise RuntimeError("A C compiler is required for the existing host JNI boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (app_classes, framework_classes, core_classes, apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-intent-candidates-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libintent_candidates_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(java_home / "include"), "-I" + str(java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("PackageManagerService", "ApkPackage"):
            relative = "app/java/com/gesher/server/" + name + ".java"
            source = ROOT / relative
            if source_ref:
                source = work / (name + ".java")
                source.write_bytes(subprocess.run(["git", "show", source_ref + ":" + relative],
                                                 cwd=ROOT, check=True, capture_output=True).stdout)
            print(name + " SHA-256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/gesher/services/IntentCandidateIndexTest.java"),
                    str(ROOT / "tests/package/IntentCandidateHost.java")]
        subprocess.run([str(java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_MICROG_SPOOF"] = "0"
        command = [str(java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                   "IntentCandidateHost", str(library)]
        if source_ref:
            command.append("baseline")
        subprocess.run(command, env=environment, check=True, timeout=120)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile prior production sources for the same contracts and workload")
    args = parser.parse_args()
    run_intent_candidates(**vars(args))


if __name__ == "__main__":
    main()
