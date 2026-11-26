#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare installed-APK storage and dynamic DEX observations."""
import argparse
import json
from pathlib import Path
import re
import sys

TAG = "GesherStorageContract"
UUID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
SHA256 = re.compile(r"[0-9a-f]{64}")
CASES = frozenset((
    "default_is_credential_protected", "created_context_is_device_protected",
    "file_paths_differ", "database_paths_differ",
    "ce_file", "de_file", "ce_preferences", "de_preferences",
    "ce_database", "de_database",
    "dex_parent_cannot_resolve_plugin", "dex_memory_loader_executes", "dex_archive_loader_executes",
    "dex_loaders_have_distinct_class_identity", "dex_invalid_memory_does_not_resolve",
    "dex_direct_slice_executes",
))
MAX_LOG_SIZE = 4 * 1024 * 1024


def observations(path, target_sdk):
    with path.open("rb") as stream:
        raw = stream.read(MAX_LOG_SIZE + 1)
    if len(raw) > MAX_LOG_SIZE:
        raise ValueError(f"{path}: capture exceeds {MAX_LOG_SIZE} bytes")
    rows = []
    for line in raw.decode("utf-8-sig").splitlines():
        match = re.fullmatch(r"(?:.*\b)?" + TAG + r"(?::)? (\{.*\})\s*", line)
        if match:
            rows.append(json.loads(match.group(1)))
    if len(rows) != 2:
        raise ValueError(f"{path}: expected two observations, found {len(rows)}")
    for phase, row in zip(("seed", "restart"), rows):
        if not isinstance(row, dict) or row.get("status") != "observed":
            raise ValueError(f"{path}: {phase} did not execute: {row}")
        checks = row.get("checks")
        if (type(row.get("schema")) is not int or row["schema"] != 1
                or type(row.get("target_sdk")) is not int or row["target_sdk"] != target_sdk
                or row.get("phase") != phase or type(row.get("pid")) is not int or row["pid"] <= 0
                or not isinstance(row.get("nonce"), str) or not UUID.fullmatch(row["nonce"])
                or not isinstance(checks, dict) or checks.keys() != CASES
                or any(type(value) is not bool for value in checks.values())):
            raise ValueError(f"{path}: invalid {phase} observation")
    if rows[0]["nonce"] != rows[1]["nonce"] or rows[0]["pid"] == rows[1]["pid"]:
        raise ValueError(f"{path}: restart must read the seed token in a different process")
    return rows


def compare(android, windows):
    if android[0]["nonce"] == windows[0]["nonce"]:
        raise ValueError("Android and Windows captures must come from independent seed runs")
    failures = []
    for reference, candidate in zip(android, windows):
        for case in sorted(CASES):
            if not reference["checks"][case] or not candidate["checks"][case]:
                failures.append(f"{reference['phase']}/{case}: "
                                f"Android={reference['checks'][case]}, Windows={candidate['checks'][case]}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android", type=Path, required=True)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--android-apk-sha256", required=True)
    parser.add_argument("--windows-apk-sha256", required=True)
    parser.add_argument("--target-sdk", type=int, default=36)
    args = parser.parse_args()
    if (not SHA256.fullmatch(args.android_apk_sha256)
            or args.android_apk_sha256 != args.windows_apk_sha256):
        parser.error("both APK captures must identify the same SHA-256")
    try:
        android = observations(args.android, args.target_sdk)
        windows = observations(args.windows, args.target_sdk)
        failures = compare(android, windows)
    except (OSError, ValueError) as problem:
        print(problem, file=sys.stderr)
        return 2
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        return 1
    print(f"PASS {2 * len(CASES)} paired storage and dynamic DEX checks across fresh processes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
