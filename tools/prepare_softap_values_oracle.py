#!/usr/bin/env python3
"""Freeze independent Android API36, pinned-r4 and candidate SoftAP/WPS payloads.

This tool never invokes ADB. It writes explicit setup/execution request arrays.
The pinned reference compiles hash-verified upstream value sources independently
of the candidate. Unavailable build annotations and real jarjar names are the
only changes to the upstream SoftAP source; its value and Parcel bodies remain.
The upstream Environment source supplies its original SDK guard. A build-only
WifiManager field declaration is excluded and resolves to the verified real
Android boot field during the reference run.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from prepare_wifi_info_oracle import jar, sha
from test_softap_values_contracts import FAMILY, ROOT, check_declarations

COMMIT = "e3f09203409da7a91d2c5cf9f0109da6d5243319"
UPSTREAM = {
    "SoftApConfiguration.java": "3c5d8887e861393f3d83a3dbcd4eb9fcfba552ce49a1812ee595c1cb58990f2a",
    "WpsInfo.java": "8e0f8020e96e6a2fe5365df41c8e05d92e5b3da6f03196f04547ffccbe8dbc50",
    "util/Environment.java": "883021cad62185fd0ff650ef7b309888f4661268179b0f48b61caac3dd6a106d",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes",
                 "d8-jar", "aosp-source-dir", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    paths = {name: out / name for name in ("candidate-classes", "sdk-callers", "launcher-classes",
             "upstream-sources", "upstream-classes", "compile-only-sources", "compile-only-classes")}
    for path in paths.values():
        path.mkdir()
    commands = []

    def run(command: list[str]) -> None:
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with (out / "build.log").open("a") as log:
            log.write(result.stdout)
        if result.returncode:
            raise RuntimeError(f"Oracle build command {len(commands)} failed: {result.stdout}")

    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    common = [javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8"]
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    fixture = ROOT / "tests/dex-services/gesher/services/SoftApValuesContractTest.java"
    launcher = ROOT / "tests/softap-values/SoftApValuesOracle.java"
    sources = [ROOT / "app/java/android/net" / (name + ".java") for name in FAMILY]
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(paths["sdk-callers"]), str(fixture)])
    run([*common, "-cp", classpath, "-d", str(paths["candidate-classes"]), *map(str, sources)])
    inventory = check_declarations(args.sdk_jar, paths["candidate-classes"])
    (out / "sdk-members.json").write_text(json.dumps(inventory, indent=2) + "\n")
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(paths["launcher-classes"]), str(launcher)])

    receipts = {}
    for relative, expected in UPSTREAM.items():
        original = args.aosp_source_dir / relative
        if sha(original) != expected:
            raise RuntimeError("Pinned upstream source mismatch: " + str(original))
        raw = original.read_text()
        adapted = re.sub(
            r"(?m)^import (android\.annotation\.FlaggedApi|androidx\.annotation\.(Keep|RequiresApi|VisibleForTesting)|com\.android\.wifi\.flags\.Flags);\n",
            "", raw)
        adapted, count = re.subn(
            r"(?m)^[ \t]*@(FlaggedApi|RequiresApi|Keep|VisibleForTesting)(\([^\n]*\))?[ \t]*\n", "", adapted)
        adapted = adapted.replace("import com.android.modules.utils.build.SdkLevel;",
                                  "import android.internal.modules.utils.build.SdkLevel;\nimport android.internal.wifi.WifiAnnotations;")
        destination = paths["upstream-sources"] / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(adapted)
        receipts[relative] = {
            "url": "https://android.googlesource.com/platform/packages/modules/Wifi/+/" + COMMIT
                   + "/framework/java/android/net/wifi/" + relative,
            "raw_sha256": expected, "compiled_source_sha256": sha(destination),
            "removed_build_annotations": count,
            "adaptations": ["Unavailable build annotations removed", "Genuine framework jarjar imports"],
        }
    # Neither this class nor its uninitialized field enters an executable payload.
    declaration = paths["compile-only-sources"] / "WifiManager.java"
    declaration.write_text("package android.net.wifi; public class WifiManager { "
                           "public static android.net.MacAddress ALL_ZEROS_MAC_ADDRESS; "
                           'public static final String UNKNOWN_SSID = "<unknown ssid>"; }\n')
    run([*common, "-cp", classpath, "-d", str(paths["compile-only-classes"]), str(declaration)])
    run([*common, "-cp", str(paths["compile-only-classes"]) + os.pathsep + classpath,
         "-d", str(paths["upstream-classes"]),
         *(str(paths["upstream-sources"] / name) for name in UPSTREAM)])
    caller_entries = {str(p.relative_to(paths["sdk-callers"])): p for p in paths["sdk-callers"].rglob("*.class")}
    candidate_entries = {str(p.relative_to(paths["candidate-classes"])): p
                         for p in paths["candidate-classes"].rglob("*.class")}
    upstream_entries = {str(p.relative_to(paths["upstream-classes"])): p
                        for p in paths["upstream-classes"].rglob("*.class")}
    launcher_entries = {str(p.relative_to(paths["launcher-classes"])): p
                        for p in paths["launcher-classes"].rglob("*.class")}
    excluded = {str(p.relative_to(paths["compile-only-classes"]))
                for p in paths["compile-only-classes"].rglob("*.class")}
    payloads = {"reference": caller_entries, "candidate": {**candidate_entries, **caller_entries},
                "pinned-r4": {**upstream_entries, **caller_entries}, "launcher": launcher_entries}
    for name, entries in payloads.items():
        if excluded.intersection(entries):
            raise RuntimeError("Compile-only declaration leaked into " + name)
        jar(out / (name + "-classes.jar"), entries)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8",
             "--min-api", "36", "--lib", str(args.sdk_jar.resolve()),
             "--output", str(out / (name + ".jar")), str(out / (name + "-classes.jar"))])
    manifest = {
        "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
        "source_files": {str(p.relative_to(ROOT)): sha(p) for p in [
            fixture, launcher, *sources, ROOT / "tools/prepare_softap_values_oracle.py",
            ROOT / "tools/test_softap_values_contracts.py"]},
        "aosp_wifi_commit": COMMIT, "upstream_sources": receipts,
        "scope": "Real native Parcel on Android; stock API36 module is version-qualified separately from pinned r4",
        "compile_only_excluded": sorted(excluded),
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar),
        "payload_classes": {name: {item: sha(path) for item, path in sorted(entries.items())}
                            for name, entries in payloads.items()},
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(out.glob("*.jar"))},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-softap-values-" + sha(out / "candidate.jar")[:12]
    setup = {"description": "Install immutable SoftAP/WPS reference and candidate payloads", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 90}
          for name in payloads),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
    ]}
    (out / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    for mode in ("reference", "candidate", "pinned-r4"):
        request = {"description": "SoftAP/WPS " + mode + " with genuine Android native Parcel", "steps": [
            {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                     "gesher.services.SoftApValuesOracle", mode, remote + "/" + mode + ".jar"], "timeout": 180}]}
        (out / (mode + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps({"output": str(out), "remote": remote, "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
