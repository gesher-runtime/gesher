#!/usr/bin/env python3
"""Compile complete PM/URI services and exercise registry-sensitive grant retirement.

The maintained JNI adapter supplies native initialization and caller identity;
all package publication, permission, provider, owner and grant logic is real.
An optional source ref runs the identical fixture against unchanged production.
"""
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
PRODUCTION = tuple("app/java/com/gesher/server/" + name + ".java" for name in
                   ("PackageManagerService", "UriGrants", "ContentProviders", "UriPermissionModes"))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transform(source: bytes, name: str, args: argparse.Namespace) -> bytes:
    text = source.decode()
    if name == "PackageManagerService.java" and args.mutation == "no-generation-rotation":
        old = "        mPackageGeneration = new Object();"
        assert text.count(old) == 1
        text = text.replace(old, "        if (mPackageGeneration == null) mPackageGeneration = new Object();")
    if name == "UriGrants.java":
        mutations = {
            "global-retirement-token": ("        Object retiredPackageGeneration;", "        static Object retiredPackageGeneration;"),
            "ignore-hosted": ("\n                && !grants.hasHostedTransientSource", ""),
            "skip-null": ("generation != null && generation == grants.retiredPackageGeneration", "generation == grants.retiredPackageGeneration"),
        }
        if args.mutation in mutations:
            old, new = mutations[args.mutation]
            assert text.count(old) == 1
            text = text.replace(old, new)
        if args.count_work:
            declaration = "public final class UriGrants {"
            assert text.count(declaration) == 1
            text = text.replace(declaration, declaration + "\n    public static long retirementVisits;")
            start = text.index("    private void retireStaleTransientsLocked(")
            end = text.index("\n    int check(", start)
            body = text[start:end]
            statement = "            Permission permission = it.next();"
            assert body.count(statement) == 1
            text = text[:start] + body.replace(statement, statement + "\n            retirementVisits++;") + text[end:]
    return text.encode()


def execute(args: argparse.Namespace, work: Path) -> None:
    work.mkdir(parents=True, exist_ok=True)
    classes = work / "classes"
    classes.mkdir(exist_ok=True)
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    library = work / "liburi_retirement_host.so"
    cc = shutil.which("cc")
    if cc is None:
        raise RuntimeError("A C compiler is required for the declared JNI boundary")
    subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                    "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                    str(ROOT / "tests/uri/intent_framework_host.c"), "-o", str(library)], check=True)
    sources: list[Path] = []
    identities: dict[str, str] = {}
    originals: dict[str, str] = {}
    for relative in PRODUCTION:
        original = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT)
                    if args.source_ref else (ROOT / relative).read_bytes())
        source = work / "production" / Path(relative).name
        source.parent.mkdir(exist_ok=True)
        originals[relative] = hashlib.sha256(original).hexdigest()
        source.write_bytes(transform(original, source.name, args))
        identities[relative] = digest(source)
        sources.append(source)
    sources += [ROOT / "tests/dex-services/gesher/services" / (name + ".java") for name in
                ("UriGrantRetirementContractTest", "UriGrantContractTest", "IntentUriGrantContractTest",
                 "PersistableUriGrantContractTest")]
    sources += [ROOT / "tests/uri/UriRetirementHost.java", ROOT / "tests/uri/UriPermissionModesOracleTest.java",
                ROOT / "tests/uri/UriRetirementWorkTest.java", ROOT / "tests/uri/UriRetirementBenchmark.java",
                ROOT / "tests/uri/UriRetirementCaller.java", ROOT / "tests/uri/UriRetirementSizeAgent.java"]
    sources += sorted((ROOT / "tests/uri/aosp").glob("*.java"))
    for path in sources:
        if path.is_relative_to(ROOT):
            identities[str(path.relative_to(ROOT))] = digest(path)
    command = [str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-source", "17", "-target", "17",
               "-cp", classpath, "-d", str(classes), *map(str, sources)]
    subprocess.run(command, check=True, timeout=90)
    cleaner = work / "cleaner/sun/misc"
    cleaner.mkdir(parents=True, exist_ok=True)
    for path in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
        shutil.copy2(path, cleaner / path.name)
    java = [str(args.java_home / "bin/java"), "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
            "-Djava.io.tmpdir=" + str(work), "-cp", str(classes) + os.pathsep + classpath]
    if args.storage:
        manifest = work / "agent-manifest.txt"
        manifest.write_text("Manifest-Version: 1.0\nPremain-Class: gesher.services.UriRetirementSizeAgent\n\n")
        agent = work / "size-agent.jar"
        subprocess.run([str(args.java_home / "bin/jar"), "--create", "--file", str(agent),
                        "--manifest", str(manifest), "-C", str(classes),
                        "gesher/services/UriRetirementSizeAgent.class"], check=True)
        java.insert(1, "-javaagent:" + str(agent))
    receipt = {"source_ref": args.source_ref, "production_and_fixtures": identities,
               "original_production": originals, "count_work": args.count_work, "mutation": args.mutation,
               "app_classes_sha256": digest(args.app_classes), "apksig_sha256": digest(args.apksig),
               "native_adapter_sha256": digest(ROOT / "tests/uri/intent_framework_host.c"),
               "native_library_sha256": digest(library), "javac": command, "java": java,
               "framework_classes": str(args.framework_classes), "core_classes": str(args.core_classes),
               "classes": {str(path.relative_to(classes)): digest(path) for path in sorted(classes.rglob("*.class"))}}
    (work / "compiled.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if args.compile_only:
        return
    environment = os.environ.copy()
    environment["ANDROID_DATA"] = str(work / "data")
    environment["GESHER_MICROG_SPOOF"] = "1"
    modes = (["--benchmark", args.benchmark] if args.benchmark else ["--storage"] if args.storage else
             ["--count-work", args.count_work] if args.count_work else
             ["--existing-contracts"] if args.existing_contracts else [])
    subprocess.run(java + ["UriRetirementHost", str(library), *modes], env=environment, check=True, timeout=90)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--existing-contracts", action="store_true")
    parser.add_argument("--count-work", choices=("candidate", "baseline"))
    parser.add_argument("--mutation", choices=("no-generation-rotation", "global-retirement-token", "ignore-hosted", "skip-null"))
    parser.add_argument("--benchmark")
    parser.add_argument("--storage", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        execute(args, args.work_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="gesher-uri-retirement-") as directory:
            execute(args, Path(directory))


if __name__ == "__main__":
    main()
