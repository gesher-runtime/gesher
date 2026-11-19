#!/usr/bin/env python3
"""Bind two complete Wi-Fi values to pinned AOSP, allowing build annotations only.

This is a source-preservation check, not a runtime support percentage. No
manager, radio, regulatory decision, blocklist or substitute Parcel is supplied.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

TOKENS = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|'
    r'[A-Za-z_$][A-Za-z0-9_$]*|[0-9]+|>>>=|>>>|>>=|<<=|>>|<<|'
    r'\+\+|--|&&|\|\||==|!=|<=|>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|->|::|[^\s]'
)


def tokens(text: str) -> list[str]:
    return [item for item in TOKENS.findall(text) if not item.startswith(("//", "/*"))]

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "app/java/android/net/wifi"
WIFI_COMMIT = "e3f09203409da7a91d2c5cf9f0109da6d5243319"
UPSTREAM = {
    "WifiAvailableChannel.java": "4c78e3ac60acc19d733807f65171cde64fe47d471abed47911c4038f3f30cc92",
    "BlockingOption.java": "ef7dd6e0d2e8d77b1b82cee708c4456c1c182bfe4699d0c3e257e6f68ca35200",
}
SUPPORT = {
    "WifiAnnotations.java": "978d080c60253390e4257a44190b177211b4f2022b0f5d8574fce47cca01909a",
    "ScanResult.java": "ca6c29c8c119dc6ba22fbd4c92a2e4f76a3f4a5b6d894f3d81ba6f89832d1875",
}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def adapt_value(source: str, name: str) -> tuple[str, dict]:
    if name not in UPSTREAM:
        raise ValueError("Unknown complete value: " + name)
    source, imports = re.subn(
        r"(?m)^import (?:android\.annotation\.FlaggedApi|com\.android\.wifi\.flags\.Flags);\n", "", source)
    source, flags = re.subn(r"(?m)^[ \t]*@FlaggedApi\([^\n]*\)[ \t]*\n", "", source)
    width = source.count("@WifiAnnotations.ChannelWidth ")
    source = source.replace("@WifiAnnotations.ChannelWidth ", "")
    nonnull = 0
    if name == "BlockingOption.java":
        if source.count("import androidx.annotation.NonNull;\n") != 1:
            raise AssertionError("AndroidX import census changed")
        source = source.replace("import androidx.annotation.NonNull;\n", "")
        source, standalone = re.subn(r"(?m)^[ \t]*@NonNull[ \t]*\n", "", source)
        nonnull = standalone + source.count("@NonNull ")
        source = source.replace("@NonNull ", "")
    expected = (2, 1, 3, 0) if name == "WifiAvailableChannel.java" else (2, 2, 0, 4)
    if (imports, flags, width, nonnull) != expected:
        raise AssertionError("Enumerated build annotation census changed: " + name)
    return source, {"removed_flag_imports": imports, "removed_flag_annotations": flags,
                    "removed_source_channel_width_annotations": width,
                    "removed_androidx_nonnull": nonnull}


def audit(upstream_dir: Path) -> dict:
    for name, expected in {**UPSTREAM, **SUPPORT}.items():
        if sha(upstream_dir / name) != expected:
            raise AssertionError("Pinned complete source changed: " + name)
    annotations = " ".join(tokens((upstream_dir / "WifiAnnotations.java").read_text(encoding="utf-8")))
    if "@ Retention ( RetentionPolicy . SOURCE ) public @ interface ChannelWidth { }" not in annotations:
        raise AssertionError("Removed ChannelWidth annotation is not SOURCE-retained")
    constant = ["public", "static", "final", "int", "CHANNEL_WIDTH_20MHZ", "=", "0", ";"]
    for path in (upstream_dir / "ScanResult.java", SOURCE_DIR / "ScanResult.java"):
        current = tokens(path.read_text(encoding="utf-8"))
        if sum(current[i:i + len(constant)] == constant for i in range(len(current))) != 1:
            raise AssertionError("Genuine ScanResult compile-time constant changed")
    rows = {}
    for name, expected in UPSTREAM.items():
        raw, current = upstream_dir / name, SOURCE_DIR / name
        original, candidate = raw.read_text(encoding="utf-8"), current.read_text(encoding="utf-8")
        adapted, changes = adapt_value(original, name)
        expected_tokens = tokens(adapted)
        if tokens(candidate) != expected_tokens or original.split("*/", 1)[0] != candidate.split("*/", 1)[0]:
            raise AssertionError("Unlisted complete-source or license difference: " + name)
        rows[name] = {"raw_sha256": expected, "candidate_sha256": sha(current), **changes,
                      "token_count": len(expected_tokens), "complete_tokens_match": True,
                      "token_sha256": hashlib.sha256("\n".join(expected_tokens).encode()).hexdigest()}
    return {"wifi_commit": WIFI_COMMIT, "scope": "Complete source preservation, not runtime coverage",
            "files": rows, "support_sources": SUPPORT, "scan_result_sha256": sha(SOURCE_DIR / "ScanResult.java"),
            "scan_result_compile_constant": {"CHANNEL_WIDTH_20MHZ": 0},
            "new_runtime_helpers": [], "manager_or_radio_implementation": False}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(audit(args.upstream_dir), indent=2) + "\n", encoding="utf-8", newline="\n")
    print("Two complete channel/blocking value sources match pinned Android16-r4 tokens")


if __name__ == "__main__":
    main()
