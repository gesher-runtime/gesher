#!/usr/bin/env python3
"""Bind an installed e2e runtime to the exact CI installer and its build inputs."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys


RECEIPT_NAME = "gesher-runtime-provenance.json"
RECEIPT_MAX_BYTES = 16 * 1024
# Paths relative to both the verified installer staging tree and installation.
# Cover the launcher, ART and framework, as well as the major native providers.
DIST_FILES = (
    "binderd.exe", "build.prop", "core-icu4j.jar", "core-libart.jar", "core-oj.jar",
    "dalvikvm.exe", "framework-res.apk", "framework.jar",
    "framework/x86_64/boot-framework.oat", "gesher-app.jar", "gesher-exec.exe",
    "gesher-launch.exe", "gesher_arm64.dll", "libEGL.dll", "libGLESv2.dll",
    "servicemanager.exe",
)
PACKAGE_FILES = ("gesher-app.cmd", "gesher-app.ps1")
REQUIRED_FILES = frozenset(DIST_FILES + PACKAGE_FILES)
SHA256 = re.compile(r"[0-9a-f]{64}")
SHA1 = re.compile(r"[0-9a-f]{40}")


def file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def matches(pattern: re.Pattern, value: object) -> bool:
    return isinstance(value, str) and pattern.fullmatch(value) is not None


def ci_source() -> str:
    source = os.environ.get("GITHUB_SHA")
    if os.environ.get("GITHUB_ACTIONS") != "true" or not matches(SHA1, source):
        raise ValueError("runtime provenance requires the GitHub Actions checkout revision")
    return source


def create_receipt(dist: Path, package: Path, setup: Path, source: str) -> dict:
    if not matches(SHA1, source):
        raise ValueError("invalid installer source revision")
    files = {name: file_sha256(dist / name) for name in DIST_FILES}
    files.update({name: file_sha256(package / name) for name in PACKAGE_FILES})
    return {
        "schema_version": 1,
        "source_revision": source,
        "installer_sha256": file_sha256(setup),
        "files_sha256": files,
    }


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate runtime provenance field")
        result[key] = value
    return result


def verify_runtime(installed: Path, setup: Path, expected_source: str) -> dict:
    with (setup.parent / RECEIPT_NAME).open("rb") as stream:
        receipt_bytes = stream.read(RECEIPT_MAX_BYTES + 1)
    if len(receipt_bytes) > RECEIPT_MAX_BYTES:
        raise ValueError("package runtime provenance is too large")
    try:
        receipt = json.loads(receipt_bytes, object_pairs_hook=unique_object)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("malformed package runtime provenance") from error
    if (not isinstance(receipt, dict)
            or receipt.keys() != {"schema_version", "files_sha256", "source_revision", "installer_sha256"}
            or type(receipt.get("schema_version")) is not int or receipt["schema_version"] != 1
            or not matches(SHA1, receipt["source_revision"])
            or not matches(SHA256, receipt["installer_sha256"])):
        raise ValueError("malformed package runtime provenance")
    if receipt["source_revision"] != expected_source:
        raise ValueError("installer belongs to a different checkout revision")
    installer_sha256 = file_sha256(setup)
    if receipt["installer_sha256"] != installer_sha256:
        raise ValueError("installer bytes differ from package provenance")
    files = receipt["files_sha256"]
    if (not isinstance(files, dict) or files.keys() != REQUIRED_FILES
            or any(not matches(SHA256, digest) for digest in files.values())):
        raise ValueError("package runtime provenance lacks the required component fingerprints")
    observed = {name: file_sha256(installed / name) for name in sorted(REQUIRED_FILES)}
    wrong = [name for name, digest in observed.items() if files[name] != digest]
    if wrong:
        raise ValueError("installed runtime bytes differ from the package inputs: " + ", ".join(wrong))
    return {
        "source_revision": receipt["source_revision"],
        "installer_sha256": installer_sha256,
        "installed_files": observed,
        "provenance_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("capture")
    capture.add_argument("--dist", required=True, type=Path)
    capture.add_argument("--package", required=True, type=Path)
    capture.add_argument("--installer", required=True, type=Path)
    verify = commands.add_parser("verify")
    verify.add_argument("--installed-runtime", required=True, type=Path)
    verify.add_argument("--installer", required=True, type=Path)
    args = parser.parse_args()
    try:
        source = ci_source()
        if args.command == "capture":
            receipt = create_receipt(args.dist, args.package, args.installer, source)
            (args.installer.parent / RECEIPT_NAME).write_text(
                json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        else:
            print(json.dumps(verify_runtime(args.installed_runtime, args.installer, source)))
    except (OSError, ValueError) as error:
        print(f"Runtime provenance: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
