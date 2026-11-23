#!/usr/bin/env python3
"""Build SDK36 reference/candidate network-specifier payloads for a real API36 device.

This prepares explicit ADB requests; it never starts an emulator or invokes ADB.
Public caller sites use only the official SDK. The candidate owns all its Wi-Fi
value classes; the platform supplies genuine matching helpers and native Parcel.
Nested boot NetworkRequest dispatch is checked by the reference and Windows gates.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess

from prepare_wifi_info_oracle import jar, sha
from test_wifi_network_specifier_contracts import FAMILY, FIXTURES, ROOT


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
    fixtures = [ROOT / "tests/dex-services/gesher/services" / name for name in FIXTURES]
    bootstrap = ROOT / "tests/wifi-network-specifier/WifiNetworkSpecifierOracle.java"
    sources = [ROOT / "app/java/android/net" / (name + ".java") for name in FAMILY]
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    common = [javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8"]
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(callers), *map(str, fixtures)])
    run([*common, "-cp", classpath, "-d", str(classes), *map(str, sources)])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(launcher), str(bootstrap)])
    caller_entries = {str(p.relative_to(callers)): p for p in callers.rglob("*.class")}
    candidate_entries = {str(p.relative_to(classes)): p for p in classes.rglob("*.class")}
    candidate_entries.update(caller_entries)
    launcher_entries = {str(p.relative_to(launcher)): p for p in launcher.rglob("*.class")}
    for name, entries in (("reference", caller_entries), ("candidate", candidate_entries), ("launcher", launcher_entries)):
        jar(work / (name + "-classes.jar"), entries)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8",
             "--min-api", "36", "--lib", str(args.sdk_jar.resolve()), "--output", str(work / (name + ".jar")),
             str(work / (name + "-classes.jar"))])
    helpers = [ROOT / "tools/prepare_wifi_network_specifier_oracle.py",
               ROOT / "tools/test_wifi_network_specifier_contracts.py",
               ROOT / "tools/prepare_wifi_info_oracle.py", ROOT / "tools/test_wifi_info_contracts.py"]
    manifest = {
        "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
        "source_files": {str(p.relative_to(ROOT)): sha(p) for p in [*fixtures, bootstrap, *sources, *helpers]},
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar),
        "candidate_classes": {name: sha(path) for name, path in sorted(candidate_entries.items())},
        "reference_classes": {name: sha(path) for name, path in sorted(caller_entries.items())},
        "launcher_classes": {name: sha(path) for name, path in sorted(launcher_entries.items())},
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(work.glob("*.jar"))},
    }
    (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-wifispecifier-" + sha(work / "candidate.jar")[:12]
    setup = {"description": "Install immutable WifiNetworkSpecifier API36 reference/candidate payloads", "steps": [
        {"adb": ["shell", "mkdir -p " + remote], "timeout": 30},
        *({"adb": ["push", str(work / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 90}
          for name in ("reference", "candidate", "launcher")),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in ("reference", "candidate", "launcher"))], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in ("reference", "candidate", "launcher"))], "timeout": 30},
    ]}
    (work / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    for mode in ("reference", "candidate"):
        request = {"description": "WifiNetworkSpecifier " + mode + " contract with real Android native Parcel",
                   "steps": [{"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process",
                                       "/system/bin", "gesher.services.WifiNetworkSpecifierOracle", mode,
                                       remote + "/" + mode + ".jar"], "timeout": 180}]}
        (work / (mode + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps({"output": str(work), "remote": remote,
                      "candidate_classes": len(candidate_entries), "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
