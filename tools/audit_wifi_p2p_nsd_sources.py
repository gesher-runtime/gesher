#!/usr/bin/env python3
"""Verify complete pinned Wi-Fi NSD source after its enumerated build adaptations.

This is source evidence, not a runtime qualification. The helper remains an
existing complete framework provider, and is never duplicated in production app.
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
CONNECTIVITY_COMMIT = "f930245ec39a510f37a9a7dfaded96d287491a61"
HELPER_SHA256 = "65b124f656064af1dee6de5646305ac9b06668ae7fc2a423c0a84a56ec27962f"
UPSTREAM = {
    "WifiP2pServiceInfo": "538cbd92c8bdd43932ac5bd6613145e82a74a17b5550f79d3a6fc13a28f7d2c6",
    "WifiP2pServiceRequest": "5e043beb45e51c9755f33bb2403c192a816f6174ccae367c96d33409036c08da",
    "WifiP2pDnsSdServiceInfo": "4d483ad5f1f4817eed5a8db96e880df1d2fa37e3af5524e07446c1e8748df56f",
    "WifiP2pDnsSdServiceRequest": "4282a265191a82562e71d0643c58669a3db11d78952e753cef6302c918c30dba",
    "WifiP2pUpnpServiceInfo": "6f4f2b9a513d334b967ac3fef5292dabbb64db625e44a903afef0b1e7f14374c",
    "WifiP2pUpnpServiceRequest": "bb2267a4c07d9d9ebdfd220bc6b9ca1499006a1b90c29fe2b14eb45754ccafd2",
    "WifiP2pUsdBasedServiceConfig": "b89c291dd9e5e296f880150b853a111cac1f32a22275ba09da2bdc938e0992e5",
}
NAMES = tuple(UPSTREAM)
BASES = ("WifiP2pServiceInfo", "WifiP2pServiceRequest")
SOURCE_DIR = ROOT / "app/java/android/net/wifi/p2p/nsd"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(upstream_dir: Path) -> dict:
    rows = {}
    for name, expected in UPSTREAM.items():
        raw, current = upstream_dir / (name + ".java"), SOURCE_DIR / (name + ".java")
        if sha(raw) != expected:
            raise AssertionError("Pinned upstream changed: " + name)
        original, candidate = raw.read_text(), current.read_text()
        if original.splitlines()[:15] != candidate.splitlines()[:15]:
            raise AssertionError("Original license changed: " + name)
        adapted = re.sub(
            r"(?m)^import (android\.annotation\.(?:FlaggedApi|RequiresApi)|"
            r"com\.android\.wifi\.flags\.Flags|android\.net\.wifi\.util\.Environment|"
            r"android\.net\.wifi\.p2p\.WifiP2pManager);\n", "", original)
        adapted, annotations = re.subn(
            r"(?m)^[ \t]*@(FlaggedApi|RequiresApi)\([^\n]*\)[ \t]*\n", "", adapted)
        adapted = adapted.replace("Environment.isSdkAtLeastB()", "isSdkAtLeastB()")
        adapted = adapted.replace("Flags.wifiDirectR2()", "WIFI_DIRECT_R2_ENABLED")
        compared, predicates = re.subn(
            r'    private static boolean isSdkAtLeastB\(\) \{\n'
            r'        return Build\.VERSION\.CODENAME\.equals\("Baklava"\)\n'
            r'                \|\| Build\.VERSION\.SDK_INT >= Build\.VERSION_CODES\.BAKLAVA;\n'
            r'    \}\n', "", candidate)
        compared, profiles = re.subn(
            r"    private static final boolean WIFI_DIRECT_R2_ENABLED = true;\n", "", compared)
        if predicates != int(name in BASES) or profiles != int(name in BASES):
            raise AssertionError("Release adaptation changed: " + name)
        expected_tokens = tokens(adapted)
        if expected_tokens != tokens(compared):
            raise AssertionError("Unlisted source token difference: " + name)
        rows[name] = {"raw_sha256": expected, "candidate_sha256": sha(current),
                      "removed_build_annotations": annotations,
                      "inlined_release_predicates": predicates, "read_only_profile_constants": profiles,
                      "tokens_after_listed_adaptations": len(expected_tokens),
                      "adapted_tokens_sha256": hashlib.sha256("\n".join(expected_tokens).encode()).hexdigest(),
                      "exact_remaining_token_match": True}
    helper = upstream_dir / "DnsSdTxtRecord.java"
    if sha(helper) != HELPER_SHA256:
        raise AssertionError("Pinned complete helper changed")
    if (ROOT / "app/java/com/android/net/module/util/DnsSdTxtRecord.java").exists():
        raise AssertionError("Existing framework helper must not be duplicated in app")
    return {"scope": "Complete source token comparison; no runtime coverage percentage",
            "aosp_wifi_commit": WIFI_COMMIT, "files": rows,
            "framework_helper": {"commit": CONNECTIVITY_COMMIT, "sha256": sha(helper),
                                 "new_production_definition": False},
            "preserved": "All fields, constructors, validation, aliases, equals/hashCode and Parcel bodies"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(audit(args.upstream_dir), indent=2) + "\n")
    print("P2P NSD complete source tokens match pinned r4: 7 files; complete existing helper")


if __name__ == "__main__":
    main()
