#!/usr/bin/env python3
"""Bind complete discovery values to pinned source and enumerated build changes.

No manager, radio implementation or replacement framework helper is supplied.
The full original readers, validation and aliasing are part of this comparison.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from audit_wifi_p2p_sources import tokens

ROOT = Path(__file__).resolve().parents[1]
WIFI_COMMIT = "e3f09203409da7a91d2c5cf9f0109da6d5243319"
MODULES_UTILS_COMMIT = "e945bbce0ccb83e1111b6031a3bba89e6b5cac6f"
SOURCE_DIR = ROOT / "app/java/android/net/wifi/p2p"
UPSTREAM = {
    "WifiP2pDiscoveryConfig.java": "44500f70612bf237044e65096eeabad7f2b558fe2439c9694e04d8100057929f",
    "WifiP2pUsdBasedLocalServiceAdvertisementConfig.java": "3ce789ce76466e28e86ce19810bd8ffc7498e53bfe946d9005893aedaec3a247",
    "WifiP2pUsdBasedServiceDiscoveryConfig.java": "37d9e2e11c94d0408c222eb01cef710cf851e2f582cec610db2836994cd33a46",
    "WifiP2pDirInfo.java": "2efeef247f555f6cbc0b20dd72a2dd0b9b6ae8da9868ca509b9ee65dee8090a4",
    "nsd/WifiP2pUsdBasedServiceResponse.java": "6075133fc229e6e9bb3ce8e7af637fb91746f88a8707034c34c5aeb5826bfd34",
}
SUPPORT = {
    "framework/java/android/net/wifi/p2p/WifiP2pManager.java": "1d54fe23a3c48fe035347aef1d8dded095bbe8ce05d83231f7cbc42c6e49175b",
    "framework/java/android/net/wifi/OuiKeyedData.java": "17c8a66b116333ce2cb454909676e6296e26496d142932fb92524a61919b6089",
    "framework/java/android/net/wifi/util/PersistableBundleUtils.java": "82a38ffde5621d1a0c7c6b8c71ce535502f8c71a68397eb8586742f171094730",
    "framework/java/android/net/wifi/ScanResult.java": "ca6c29c8c119dc6ba22fbd4c92a2e4f76a3f4a5b6d894f3d81ba6f89832d1875",
    "framework/java/android/net/wifi/WifiScanner.java": "1814e8c065636da89dfedd05dc393627ff0d87bcbe8cd41908b898b99fd0cfc9",
    "java/com/android/modules/utils/build/SdkLevel.java": "74580e2fcc7cac5b405ae727e90a1ce1530e02d944cee375513ce29ad785b929",
}
MANAGER_CONSTANTS = {"WIFI_P2P_SCAN_FULL": 0, "WIFI_P2P_SCAN_SOCIAL": 1, "WIFI_P2P_SCAN_SINGLE_FREQ": 2}
HELPERS = ("android/net/wifi/OuiKeyedData.java", "android/net/wifi/util/PersistableBundleUtils.java")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def remove_build_annotations(source: str) -> tuple[str, int]:
    source = re.sub(r"(?m)^import (?:android\.annotation\.(?:FlaggedApi|RequiresApi)|com\.android\.wifi\.flags\.Flags);\n", "", source)
    return re.subn(r"(?m)^[ \t]*@(?:FlaggedApi|RequiresApi)\([^\n]*\)[ \t]*\n", "", source)


def adapt_value(source: str, path: str, *, candidate: bool) -> tuple[str, dict]:
    source, annotations = remove_build_annotations(source)
    scan_annotations = source.count("@WifiP2pManager.WifiP2pScanType ")
    if scan_annotations != (4 if path == "WifiP2pDiscoveryConfig.java" else 0):
        raise AssertionError("SOURCE annotation census changed: " + path)
    source = source.replace("@WifiP2pManager.WifiP2pScanType ", "")
    replacements = {}
    if candidate and path == "WifiP2pDiscoveryConfig.java":
        for name, expected_count in (("WIFI_P2P_SCAN_FULL", 1), ("WIFI_P2P_SCAN_SINGLE_FREQ", 2)):
            qualified = "WifiP2pManager." + name
            if source.count(qualified) != expected_count:
                raise AssertionError("Manager constant use changed: " + name)
            source = source.replace(qualified, str(MANAGER_CONSTANTS[name]))
            replacements[name] = {"value": MANAGER_CONSTANTS[name], "count": expected_count}
    relocated = source.count("import com.android.modules.utils.build.SdkLevel;")
    if relocated != (1 if path == "WifiP2pDiscoveryConfig.java" else 0):
        raise AssertionError("SdkLevel import census changed")
    source = source.replace("import com.android.modules.utils.build.SdkLevel;",
                            "import android.internal.modules.utils.build.SdkLevel;")
    return source, {"removed_build_annotations": annotations, "removed_source_scan_annotations": scan_annotations,
                    "manager_constant_references": replacements, "framework_jarjar_imports": relocated}


def adapt_sdklevel(source: str) -> str:
    source = source.replace("package com.android.modules.utils.build;", "package android.internal.modules.utils.build;")
    source = re.sub(r"(?m)^import androidx\.annotation\.(?:ChecksSdkIntAtLeast|NonNull);\n", "", source)
    source, checks = re.subn(r"(?m)^[ \t]*@ChecksSdkIntAtLeast\([^\n]*\)[ \t]*\n", "", source)
    source, nonnull = re.subn(r"@NonNull[ \t]+", "", source)
    if checks != 7 or nonnull != 1:
        raise AssertionError("Complete SdkLevel annotation census changed")
    return source


def audit(upstream_dir: Path, support_dir: Path) -> dict:
    for relative, expected in SUPPORT.items():
        if sha(support_dir / relative) != expected:
            raise AssertionError("Pinned support source changed: " + relative)
    manager = (support_dir / next(iter(SUPPORT))).read_text(encoding="utf-8")
    for name, value in MANAGER_CONSTANTS.items():
        if not re.search(r"public static final int " + name + r" = " + str(value) + r";", manager):
            raise AssertionError("Pinned manager constant changed: " + name)
    if not re.search(r"@Retention\(RetentionPolicy\.SOURCE\)\s+public @interface WifiP2pScanType", manager):
        raise AssertionError("Manager annotation retention changed")
    rows = {}
    for relative, expected in UPSTREAM.items():
        raw, current = upstream_dir / relative, SOURCE_DIR / relative
        if sha(raw) != expected:
            raise AssertionError("Pinned value source changed: " + relative)
        original, candidate = raw.read_text(encoding="utf-8"), current.read_text(encoding="utf-8")
        if original.splitlines()[:15] != candidate.splitlines()[:15]:
            raise AssertionError("Original license changed: " + relative)
        adapted, changes = adapt_value(original, relative, candidate=True)
        expected_tokens = tokens(adapted)
        if expected_tokens != tokens(candidate):
            raise AssertionError("Unlisted source token difference: " + relative)
        rows[relative] = {"raw_sha256": expected, "candidate_sha256": sha(current), **changes,
                          "token_count": len(expected_tokens), "exact_remaining_token_match": True,
                          "adapted_tokens_sha256": hashlib.sha256("\n".join(expected_tokens).encode()).hexdigest()}
    helpers = {}
    for relative in HELPERS:
        raw, current = support_dir / "framework/java" / relative, ROOT / "app/java" / relative
        adapted, annotations = remove_build_annotations(raw.read_text(encoding="utf-8"))
        if tokens(adapted) != tokens(current.read_text(encoding="utf-8")):
            raise AssertionError("Existing complete helper no longer matches: " + relative)
        helpers[relative] = {"raw_sha256": sha(raw), "existing_sha256": sha(current),
                              "removed_build_annotations": annotations, "complete_tokens_match": True}
    band_constants = {"WIFI_BAND_24_GHZ": 1, "WIFI_BAND_5_GHZ": 2, "WIFI_BAND_6_GHZ": 8, "UNSPECIFIED": -1}
    raw_scan = (support_dir / "framework/java/android/net/wifi/ScanResult.java").read_text(encoding="utf-8")
    scanner = (support_dir / "framework/java/android/net/wifi/WifiScanner.java").read_text(encoding="utf-8")
    current_scan = (ROOT / "app/java/android/net/wifi/ScanResult.java").read_text(encoding="utf-8")
    for name, value in band_constants.items():
        declaration = r"public (?:static final|final static) int " + name + r" = "
        if not re.search(declaration + str(value) + r";", current_scan):
            raise AssertionError("Current ScanResult compile constant changed: " + name)
        if name == "UNSPECIFIED":
            if not re.search(declaration + str(value) + r";", raw_scan):
                raise AssertionError("Pinned unspecified sentinel changed")
        else:
            index = name.replace("WIFI_BAND_", "WIFI_BAND_INDEX_")
            if not re.search(declaration + r"WifiScanner\." + name + r";", raw_scan):
                raise AssertionError("Pinned ScanResult scanner constant reference changed")
            if not re.search(declaration + r"1 << " + index + r";", scanner):
                raise AssertionError("Pinned scanner band expression changed")
            match = re.search(r"public static final int " + index + r" = ([0-9]+);", scanner)
            if not match or 1 << int(match.group(1)) != value:
                raise AssertionError("Pinned scanner band index changed")
    if (SOURCE_DIR / "WifiP2pManager.java").exists():
        raise AssertionError("No fake manager may be added")
    if (ROOT / "app/java/android/internal/modules/utils/build/SdkLevel.java").exists():
        raise AssertionError("Existing framework helper must not be duplicated")
    adapt_sdklevel((support_dir / "java/com/android/modules/utils/build/SdkLevel.java").read_text(encoding="utf-8"))
    return {"scope": "Complete source token preservation; no runtime coverage percentage",
            "wifi_commit": WIFI_COMMIT, "modules_utils_commit": MODULES_UTILS_COMMIT, "files": rows,
            "manager_source_sha256": SUPPORT[next(iter(SUPPORT))], "manager_constants": MANAGER_CONSTANTS,
            "scan_result_compile_constants": band_constants,
            "existing_helpers": helpers, "sdklevel_raw_sha256": SUPPORT["java/com/android/modules/utils/build/SdkLevel.java"],
            "preserved": "All fields, constructors, validation, aliases, Object identity equality and full Parcel bodies"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("upstream-dir", "support-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(audit(args.upstream_dir, args.support_dir), indent=2) + "\n", encoding="utf-8", newline="\n")
    print("P2P discovery complete source tokens match pinned r4: five values; complete existing dependencies")


if __name__ == "__main__":
    main()
