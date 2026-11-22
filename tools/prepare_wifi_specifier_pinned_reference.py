#!/usr/bin/env python3
"""Build the pinned AOSP r4 native writer oracle independently of Gesher providers.

The stock API36 image predates three r4 WifiConfiguration tail fields. Complete
upstream specifier/agent/configuration/security sources, verified by immutable hashes,
execute their real writers on Android native Parcel. No bytes are normalized.
Build-only external declarations never enter the runtime payload.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from prepare_wifi_info_oracle import jar, sha

ROOT = Path(__file__).resolve().parents[1]
COMMIT = "e3f09203409da7a91d2c5cf9f0109da6d5243319"
SOURCES = {
    "WifiNetworkSpecifier.java": "c764ebdbdb13da5f9941fcb7aee23ea3aed6007d8d2f1c32597ebd3c4984395e",
    "WifiNetworkAgentSpecifier.java": "afbe67e09af2e9a81ed3cee2ced239e2584dfc941d080a3ae9baa00ad69ec852",
    "WifiConfiguration.java": "d1585fa9ab0ca3a9c4837920fd7e471df691fd9e0aae522b8c30f7fd9a75dac1",
    "SecurityParams.java": "9aaccf30c9a81f6365248f97d63bf633eac06d0dfdb59e260c8c384dc7ebb060",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes",
                 "d8-jar", "source-dir", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    directories = {name: out / name for name in
                   ("sources", "compile-only-sources", "compile-only-classes", "upstream-classes", "oracle-classes")}
    for path in directories.values():
        path.mkdir()
    commands = []

    def run(command: list[str]) -> None:
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        with (out / "build.log").open("a") as log:
            log.write(result.stdout)
        if result.returncode:
            raise RuntimeError(f"Upstream reference build failed: {result.stdout}")

    receipts = {}
    for name, expected in SOURCES.items():
        original = args.source_dir / ("r4-" + name)
        if sha(original) != expected:
            raise RuntimeError("Pinned upstream source mismatch: " + str(original))
        source = original.read_text()
        adapted = re.sub(r"(?m)^import (android\.annotation\.FlaggedApi|androidx\.annotation\.(Keep|RequiresApi));\n", "", source)
        adapted, count = re.subn(r"(?m)^[ \t]*@(FlaggedApi|RequiresApi|Keep)(\([^\n]*\))?[ \t]*\n", "", adapted)
        adapted = adapted.replace("import com.android.modules.utils.build.SdkLevel;",
                                  "import android.internal.modules.utils.build.SdkLevel;")
        destination = directories["sources"] / name
        destination.write_text(adapted)
        receipts[name] = {"url": "https://android.googlesource.com/platform/packages/modules/Wifi/+/" + COMMIT
                                + "/framework/java/android/net/wifi/" + name,
                          "raw_sha256": expected, "compiled_source_sha256": sha(destination),
                          "removed_build_annotations": count,
                          "adaptations": ["Unavailable build annotations removed", "Genuine framework jarjar SdkLevel import"]}

    declarations = {
        "Flags.java": "package com.android.wifi.flags; public final class Flags { public static native boolean multiUserWifiEnhancement(); }\n",
        "Environment.java": "package android.net.wifi.util; public final class Environment { public static native boolean isSdkNewerThanB(); }\n",
        "WifiManager.java": "package android.net.wifi; public class WifiManager { public static android.net.MacAddress ALL_ZEROS_MAC_ADDRESS; }\n",
    }
    # Flags/Environment are referenced only by unexecuted API37 identity methods.
    # The executed manager constant comes from the verified Android boot provider.
    for name, content in declarations.items():
        (directories["compile-only-sources"] / name).write_text(content)
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    common = [javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8"]
    run([*common, "-cp", classpath, "-d", str(directories["compile-only-classes"]),
         *(str(directories["compile-only-sources"] / name) for name in declarations)])
    run([*common, "-cp", str(directories["compile-only-classes"]) + os.pathsep + classpath,
         "-d", str(directories["upstream-classes"]), *(str(directories["sources"] / name) for name in SOURCES)])
    oracle = ROOT / "tests/wifi-network-specifier/PinnedWifiSpecifierWireOracle.java"
    certificates = ROOT / "tests/dex-services/gesher/services/WifiSpecifierCertificates.java"
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(directories["oracle-classes"]),
         str(oracle), str(certificates)])
    upstream_entries = {str(p.relative_to(directories["upstream-classes"])): p
                        for p in directories["upstream-classes"].rglob("*.class")}
    oracle_entries = {str(p.relative_to(directories["oracle-classes"])): p
                      for p in directories["oracle-classes"].rglob("*.class")}
    excluded = {str(p.relative_to(directories["compile-only-classes"]))
                for p in directories["compile-only-classes"].rglob("*.class")}
    payload_entries = {**upstream_entries, **oracle_entries}
    if excluded.intersection(payload_entries):
        raise RuntimeError("Compile-only declarations leaked into the oracle")
    for name, entries in (("reference", payload_entries), ("launcher", oracle_entries)):
        jar(out / (name + "-classes.jar"), entries)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8",
             "--min-api", "36", "--lib", str(args.sdk_jar.resolve()),
             "--output", str(out / (name + ".jar")), str(out / (name + "-classes.jar"))])
    manifest = {"aosp_wifi_commit": COMMIT, "upstream_sources": receipts,
                "scope": "Pinned upstream constructors and native writers; not a replacement Android system image",
                "source_files": {str(p.relative_to(ROOT)): sha(p) for p in
                                 (oracle, certificates, ROOT / "tools/prepare_wifi_specifier_pinned_reference.py")},
                "upstream_classes": {name: sha(p) for name, p in sorted(upstream_entries.items())},
                "oracle_classes": {name: sha(p) for name, p in sorted(oracle_entries.items())},
                "compile_only_excluded": sorted(excluded),
                "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size}
                              for p in sorted(out.glob("*.jar"))},
                "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar)}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-wifi-r4-reference-" + sha(out / "reference.jar")[:12]
    setup = {"description": "Install pinned upstream r4 native writer reference", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 90}
          for name in ("reference", "launcher")),
        {"adb": ["shell", "sha256sum", remote + "/reference.jar", remote + "/launcher.jar"], "timeout": 30}]}
    request = {"description": "Execute actual pinned upstream r4 writers against Android native Parcel", "steps": [
        {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                 "gesher.services.PinnedWifiSpecifierWireOracle", remote + "/reference.jar"], "timeout": 180}]}
    (out / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    (out / "reference-request.json").write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps({"output": str(out), "remote": remote, "upstream_classes": len(upstream_entries),
                      "excluded": sorted(excluded), "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
