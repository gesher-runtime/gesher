#!/usr/bin/env python3
"""Freeze stock API36, independent pinned-r4 and candidate Suggestion/Passpoint payloads.

No ADB, emulator lifecycle or system-image changes. Public callers compile solely
against SDK36. Pinned sources are hash-verified and compiled independently. Their
nested readers select the defining value provider; complete writer bodies remain
unchanged. Exact raw-to-reference diffs and excluded compile-only declarations are
saved. A source-owned WifiConfiguration/SecurityParams pair prevents mixed helper
signatures. All providers and parameter/return identities are asserted at runtime.
"""
from __future__ import annotations

import argparse
import json
import difflib
import os
from pathlib import Path
import re
import subprocess

from prepare_wifi_info_oracle import jar, sha
from test_wifi_suggestion_values import FAMILY, FIXTURES, ROOT, check_declarations

COMMIT = "e3f09203409da7a91d2c5cf9f0109da6d5243319"
UPSTREAM = {
    "WifiNetworkSuggestion.java": "5071e5a930f6ffd88789d60f2c7f62cbdc1e069990fa1a419f5439442c47ae05",
    "hotspot2/PasspointConfiguration.java": "9365ab973f327a3e98f5cc8ff007feedd2e02b51f69fdcda228f844c13a6d828",
    "hotspot2/pps/HomeSp.java": "c23bbb343a86d36a05d5f415f634b6ef8b8209a8e70a6b863f049d507eca1ac6",
    "hotspot2/pps/Credential.java": "cef9ef27bc19398273281e11b7430fd793aacc3a3034a67e45d079ee77d357ee",
    "hotspot2/pps/Policy.java": "fd29aa4823f808a4c4a26c4d9fc8b872ef93fd3da8c259ecb7e86f77bb9c964c",
    "hotspot2/pps/UpdateParameter.java": "5b4001226a297395d6f1bb2be326eef68d930d9952957283e8274917ade7cbb1",
    "EAPConstants.java": "e56e17a3d4f77d39960be406cad7533c3b109e92aede3f9af23e134394454f96",
    "util/BuildProperties.java": "0f7925a9afba3da2f06d6b986d544da41fbb131625b4df31ac55c9f39a218c95",
    "WifiConfiguration.java": "d1585fa9ab0ca3a9c4837920fd7e471df691fd9e0aae522b8c30f7fd9a75dac1",
    "SecurityParams.java": "9aaccf30c9a81f6365248f97d63bf633eac06d0dfdb59e260c8c384dc7ebb060",
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
    fixtures = [ROOT / "tests/dex-services/gesher/services" / name for name in FIXTURES]
    launcher = ROOT / "tests/wifi-suggestion/WifiSuggestionValuesOracle.java"
    sources = [ROOT / "app/java/android/net" / (name + ".java") for name in FAMILY]
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(paths["sdk-callers"]), *map(str, fixtures)])
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
            r"(?m)^import (android\.annotation\.FlaggedApi|androidx\.annotation\.(Keep|RequiresApi|VisibleForTesting));\n",
            "", raw)
        adapted, count = re.subn(
            r"(?m)^[ \t]*@(FlaggedApi|RequiresApi|Keep|VisibleForTesting)(\([^\n]*\))?[ \t]*\n", "", adapted)
        adapted = adapted.replace("import com.android.modules.utils.build.SdkLevel;",
                                  "import android.internal.modules.utils.build.SdkLevel;\nimport android.internal.wifi.WifiAnnotations;")
        if relative != "WifiConfiguration.java":
            adapted = adapted.replace("import com.android.wifi.flags.Flags;\n", "")
        reader_count = adapted.count("in.readParcelable(null)")
        adapted = adapted.replace("in.readParcelable(null)",
                                  "in.readParcelable(" + Path(relative).stem + ".class.getClassLoader())")
        # The defining-loader choice affects only nested provider lookup. Writer bodies stay byte-for-byte unchanged.
        destination = paths["upstream-sources"] / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(adapted)
        destination.with_suffix(".diff").write_text("".join(difflib.unified_diff(
            raw.splitlines(True), adapted.splitlines(True), fromfile="aosp/" + relative, tofile="reference/" + relative)))
        receipts[relative] = {
            "url": "https://android.googlesource.com/platform/packages/modules/Wifi/+/" + COMMIT
                   + "/framework/java/android/net/wifi/" + relative,
            "raw_sha256": expected, "compiled_source_sha256": sha(destination),
            "removed_build_annotations": count, "explicit_nested_loaders": reader_count,
            "adaptations": ["Unavailable build annotations removed", "Genuine framework jarjar imports",
                            "Nested readers use their defining provider; unchanged writer bodies"],
        }
    # Neither declaration enters an executable payload. The manager constant resolves to the real
    # Android boot provider. Flags is referenced by getCreatorUserIdInternal(), reached by
    # getCreatorUserId(); neither declaration is in SDK36. These hidden reference paths are
    # outside this corpus and their runtime linkage is not qualified by payload exclusion.
    declarations = {
        "WifiManager.java": "package android.net.wifi; public class WifiManager { public static android.net.MacAddress ALL_ZEROS_MAC_ADDRESS; }\n",
        "Flags.java": "package com.android.wifi.flags; public final class Flags { public static native boolean multiUserWifiEnhancement(); }\n",
    }
    for name, body in declarations.items():
        (paths["compile-only-sources"] / name).write_text(body)
    run([*common, "-cp", classpath, "-d", str(paths["compile-only-classes"]),
         *(str(paths["compile-only-sources"] / name) for name in declarations)])
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
            *fixtures, launcher, *sources, ROOT / "tools/prepare_wifi_suggestion_oracle.py",
            ROOT / "tools/test_wifi_suggestion_values.py"]},
        "aosp_wifi_commit": COMMIT, "upstream_sources": receipts,
        "scope": "Real native Parcel on Android; stock API36 module is version-qualified separately from pinned r4",
        "compile_only_excluded": sorted(excluded),
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar),
        "payload_classes": {name: {item: sha(path) for item, path in sorted(entries.items())}
                            for name, entries in payloads.items()},
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(out.glob("*.jar"))},
    }
    # Freeze the exact input text as well as its hash, so later harness revisions
    # cannot erase the provenance of an earlier completed or failed execution.
    for relative, expected in manifest["source_files"].items():
        snapshot = out / "input-sources" / relative
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        snapshot.write_bytes((ROOT / relative).read_bytes())
        if sha(snapshot) != expected:
            raise RuntimeError("Source changed while freezing oracle input: " + relative)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-wifi-suggestion-" + sha(out / "candidate.jar")[:12]
    setup = {"description": "Install immutable Suggestion/Passpoint reference and candidate payloads", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 90}
          for name in payloads),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
    ]}
    (out / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    for mode in ("reference", "candidate", "pinned-r4"):
        request = {"description": "Suggestion/Passpoint " + mode + " with genuine Android native Parcel", "steps": [
            {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                     "gesher.services.WifiSuggestionValuesOracle", mode, remote + "/" + mode + ".jar"], "timeout": 180}]}
        (out / (mode + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps({"output": str(out), "remote": remote, "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
