#!/usr/bin/env python3
"""Verify the complete seven-source P2P port after its enumerated adaptations.

This is a source token comparison, not runtime qualification. String/character
contents remain tokens; comment markers inside literals are never stripped.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from prepare_wifi_p2p_oracle import UPSTREAM
from test_wifi_p2p_values import P2P, ROOT, sha

TOKENS = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|'
    r'[A-Za-z_$][A-Za-z0-9_$]*|[0-9]+|>>>=|>>>|>>=|<<=|>>|<<|'
    r'\+\+|--|&&|\|\||==|!=|<=|>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|->|::|[^\s]'
)


def tokens(text: str) -> list[str]:
    return [item for item in TOKENS.findall(text) if not item.startswith(("//", "/*"))]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = {}
    for name in P2P:
        raw = args.upstream_dir / (name + ".java")
        current = ROOT / "app/java/android/net/wifi/p2p" / raw.name
        if sha(raw) != UPSTREAM[raw.name]:
            raise AssertionError("Pinned upstream source changed: " + raw.name)
        original, candidate = raw.read_text(), current.read_text()
        expected_dir_uses = 1 if name == "WifiP2pDevice" else 2 if name == "WifiP2pDeviceList" else 0
        if tokens(original).count("dirInfo") != expected_dir_uses or "dirInfo" in tokens(candidate):
            raise AssertionError("Hidden DIR caller/use audit changed: " + raw.name)
        if original.splitlines()[:15] != candidate.splitlines()[:15]:
            raise AssertionError("Original license header changed: " + raw.name)
        adapted = re.sub(
            r"(?m)^import (android\.annotation\.FlaggedApi|androidx\.annotation\.RequiresApi|"
            r"com\.android\.wifi\.flags\.Flags|android\.net\.wifi\.util\.Environment);\n", "", original)
        adapted = re.sub(r"(?m)^[ \t]*@(FlaggedApi|RequiresApi)\([^\n]*\)[ \t]*\n", "", adapted)
        adapted = adapted.replace("import com.android.modules.utils.build.SdkLevel;",
                                  "import android.internal.modules.utils.build.SdkLevel;")
        adapted = adapted.replace("Environment.isSdkAtLeastB()", "isSdkAtLeastB()")
        adapted = adapted.replace("Flags.wifiDirectR2()", "WIFI_DIRECT_R2_ENABLED")
        if name == "WifiP2pDevice":
            declaration = "@Nullable\n    public WifiP2pDirInfo dirInfo;"
            if adapted.count(declaration) != 1:
                raise AssertionError("Expected exactly one hidden DIR field")
            adapted = adapted.replace(declaration, "")
        if name == "WifiP2pDeviceList":
            assignment = "d.dirInfo = device.dirInfo;"
            if adapted.count(assignment) != 1:
                raise AssertionError("Expected exactly one hidden DIR assignment")
            adapted = adapted.replace(assignment, "")
        compared, predicates = re.subn(
            r'    private static boolean isSdkAtLeastB\(\) \{\n'
            r'        return Build\.VERSION\.CODENAME\.equals\("Baklava"\)\n'
            r'                \|\| Build\.VERSION\.SDK_INT >= Build\.VERSION_CODES\.BAKLAVA;\n'
            r'    \}\n', "", candidate)
        compared, profiles = re.subn(
            r"    private static final boolean WIFI_DIRECT_R2_ENABLED = true;\n", "", compared)
        if predicates != int(name in ("WifiP2pConfig", "WifiP2pDevice", "WifiP2pDeviceList", "WifiP2pGroup")):
            raise AssertionError("Release-predicate adaptation changed: " + raw.name)
        if profiles != int(name in ("WifiP2pConfig", "WifiP2pGroup")):
            raise AssertionError("Read-only profile adaptation changed: " + raw.name)
        expected_tokens, actual_tokens = tokens(adapted), tokens(compared)
        if expected_tokens != actual_tokens:
            raise AssertionError("Unlisted source-token difference: " + raw.name)
        rows[raw.name] = {
            "raw_sha256": sha(raw), "candidate_sha256": sha(current),
            "tokens_after_listed_adaptations": len(expected_tokens),
            "exact_remaining_token_match": True, "inlined_release_predicates": predicates,
            "read_only_profile_constants": profiles,
            "adapted_tokens_sha256": hashlib.sha256("\n".join(expected_tokens).encode()).hexdigest(),
        }
    result = {
        "scope": "All non-comment Java tokens after listed adaptations; not runtime qualification",
        "files": rows,
        "hidden_dir_use_audit": {
            "raw_device_field_sites": 1, "raw_device_list_assignment_sites": 1,
            "candidate_field_or_assignment_sites": 0,
            "public_copy_parcel_or_observer_sites": 0,
            "adjacent_pairing_update_retained": True,
            "scope": "retained seven-source closure; DIR is not claimed globally unused",
        },
        "preserved_behavior": "Validation, copies, aliases, collections, Parcel readers/writers and other retained bodies have no further token changes.",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("P2P source tokens match pinned r4 after listed adaptations: 7 files")


if __name__ == "__main__":
    main()
