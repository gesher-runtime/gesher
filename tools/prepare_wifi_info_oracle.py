#!/usr/bin/env python3
"""Build immutable SDK36 reference/candidate WifiInfo payloads for a real API36 device.

This prepares explicit ADB requests; it never starts an emulator or invokes ADB.
The candidate owns every local value provider and the actual framework IPv4 helper.
Both execution modes share the same SDK-compiled corpus and genuine native Parcel.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

from test_wifi_info_contracts import FAMILY, ROOT


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def jar(path: Path, entries: dict[str, Path]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as output:
        for name, source in sorted(entries.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            output.writestr(info, source.read_bytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes", "d8-jar", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    work = args.output_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(command: list[str]) -> None:
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (work / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with (work / "build.log").open("a") as log:
            log.write(result.stdout)
        if result.returncode:
            raise RuntimeError(f"Build command {len(commands)} exited {result.returncode}: {result.stdout}")

    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    classes, callers, launcher = (work / name for name in ("candidate-classes", "sdk-callers", "launcher-classes"))
    for path in (classes, callers, launcher):
        path.mkdir()
    fixture = ROOT / "tests/dex-services/gesher/services/WifiInfoContractTest.java"
    bootstrap = ROOT / "tests/wifi-info/WifiInfoOracle.java"
    sources = [ROOT / "app/java/android/net" / (name + ".java") for name in FAMILY]
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    common = [javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8"]
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(callers), str(fixture)])
    run([*common, "-cp", classpath, "-d", str(classes), *map(str, sources)])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(launcher), str(bootstrap)])
    helper_name = "com/android/net/module/util/Inet4AddressUtils.class"
    helper = args.framework_classes.resolve() / helper_name
    if not helper.is_file():
        raise RuntimeError("The real framework IPv4 helper class is required")
    helper_output = classes / helper_name
    helper_output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(helper, helper_output)
    caller_entries = {str(p.relative_to(callers)): p for p in callers.rglob("*.class")}
    candidate_entries = {str(p.relative_to(classes)): p for p in classes.rglob("*.class")}
    candidate_entries.update(caller_entries)
    launcher_entries = {str(p.relative_to(launcher)): p for p in launcher.rglob("*.class")}
    for name, entries in (("reference", caller_entries), ("candidate", candidate_entries), ("launcher", launcher_entries)):
        jar(work / (name + "-classes.jar"), entries)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8",
             "--min-api", "36", "--lib", str(args.sdk_jar.resolve()), "--output", str(work / (name + ".jar")),
             str(work / (name + "-classes.jar"))])
    manifest = {
        "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
        "source_files": {str(p.relative_to(ROOT)): sha(p) for p in [fixture, bootstrap, *sources,
                         ROOT / "tools/prepare_wifi_info_oracle.py", ROOT / "tools/test_wifi_info_contracts.py"]},
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar),
        "framework_helper": {"path": str(helper), "sha256": sha(helper)},
        "candidate_classes": {name: sha(path) for name, path in sorted(candidate_entries.items())},
        "reference_classes": {name: sha(path) for name, path in sorted(caller_entries.items())},
        "launcher_classes": {name: sha(path) for name, path in sorted(launcher_entries.items())},
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(work.glob("*.jar"))},
    }
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-wifiinfo-" + sha(work / "candidate.jar")[:12]
    setup = {"description": "Install immutable WifiInfo API36 reference/candidate payloads", "steps": [
        {"adb": ["shell", "mkdir -p " + remote], "timeout": 30},
        *({"adb": ["push", str(work / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 90}
          for name in ("reference", "candidate", "launcher")),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in ("reference", "candidate", "launcher"))], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in ("reference", "candidate", "launcher"))], "timeout": 30},
    ]}
    (work / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    for mode in ("reference", "candidate"):
        request = {"description": "WifiInfo " + mode + " contract with real Android native Parcel",
                   "steps": [{"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process",
                                       "/system/bin", "gesher.services.WifiInfoOracle", mode,
                                       remote + "/" + mode + ".jar"], "timeout": 180}]}
        (work / (mode + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps({"output": str(work), "remote": remote,
                      "candidate_classes": len(candidate_entries), "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
