#!/usr/bin/env python3
"""Measure a pinned APK's identity with Android SDK tools; never execute the APK.

The SDK parser and signature verifier inspect a private copy of the same pinned
bytes. Native-code entries are packaged ABIs, not evidence of the runtime's ABI
selection or successful execution. This collector does not establish compatibility.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HASH = re.compile(r"[0-9a-fA-F]{64}\Z")
MAX_TOOL_OUTPUT = 4 * 1024 * 1024


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_tools(explicit: Path | None = None) -> Path:
    """Use an explicit installation or the newest installed stable SDK tools.

    No download and no PATH fallback: aapt2 and apksigner must come from the
    same named build-tools directory, and their actual bytes are recorded.
    """
    candidates = []
    if explicit:
        candidates = [explicit]
    else:
        for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
            if os.environ.get(variable):
                root = Path(os.environ[variable]) / "build-tools"
                if root.is_dir():
                    candidates.extend(item for item in root.iterdir()
                                      if item.is_dir() and re.fullmatch(r"\d+(?:\.\d+)+", item.name))
        candidates.sort(key=lambda item: tuple(map(int, item.name.split("."))), reverse=True)
    executable = "aapt2.exe" if os.name == "nt" else "aapt2"
    for directory in candidates:
        if (directory / executable).is_file() and (directory / "lib/apksigner.jar").is_file():
            return directory.resolve()
    raise ValueError("Android SDK build-tools with aapt2 and lib/apksigner.jar are required")


def run_tool(argv: list[str], role: str, timeout: int = 45) -> str:
    # Disk-backed output avoids unbounded capture in memory for a malformed APK.
    # External tools have a deadline; never invoke a shell or execute APK code.
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        try:
            result = subprocess.run(argv, stdout=output, stderr=errors, timeout=timeout, check=False)
        except subprocess.TimeoutExpired as error:
            raise ValueError(f"{role} exceeded its {timeout}s deadline") from error
        if result.returncode:
            # Tool output can contain machine paths or APK-controlled strings.
            # Preserve only the outcome in the structured public evidence.
            raise ValueError(f"{role} failed (exit {result.returncode})")
        if output.tell() > MAX_TOOL_OUTPUT or errors.tell() > MAX_TOOL_OUTPUT:
            raise ValueError(f"{role} output exceeded the metadata limit")
        output.seek(0)
        return output.read().decode("utf-8", errors="strict")


def parse_badging(text: str) -> dict:
    lines = text.splitlines()
    package_lines = [line for line in lines if line.startswith("package:")]
    if len(package_lines) != 1:
        raise ValueError("aapt2 did not return one package identity")
    # Values printed by aapt2 may contain escaped quotes, so only parse the
    # machine identifiers and numeric versionCode fields, not display labels.
    line = package_lines[0]
    package = re.search(r"(?:^|\s)name='([A-Za-z0-9_.]+)'(?:\s|$)", line)
    version = re.search(r"(?:^|\s)versionCode='([0-9]*)'(?:\s|$)", line)
    major = re.search(r"(?:^|\s)versionCodeMajor='([0-9]+)'(?:\s|$)", line)
    if not package or not version:
        raise ValueError("aapt2 package name or versionCode is malformed")
    low, high = int(version[1]) if version[1] else None, int(major[1]) if major else 0
    if (low is not None and low > 0xffffffff) or high > 0x7fffffff:
        raise ValueError("aapt2 versionCode is outside the Android long-version range")

    def sdk(prefix):
        found = [item for item in lines if item.startswith(prefix + ":")]
        if not found:
            return None
        if len(found) != 1:
            raise ValueError(f"aapt2 repeated {prefix}")
        match = re.fullmatch(re.escape(prefix) + r":'([A-Za-z0-9_.]+)'", found[0])
        if not match:
            raise ValueError(f"aapt2 returned malformed {prefix}")
        value = match[1]
        return int(value) if value.isdecimal() else value

    abis = []
    for kind in ("native-code", "alt-native-code"):
        native = [item for item in lines if item.startswith(kind + ":")]
        if len(native) > 1:
            raise ValueError(f"aapt2 repeated {kind}")
        if not native:
            continue
        if kind == "alt-native-code" and not abis:
            raise ValueError("aapt2 returned alternate ABIs without a primary ABI")
        values = native[0][len(kind) + 1:].strip()
        if not re.fullmatch(r"(?:'[A-Za-z0-9_-]+'(?:\s+|$))+", values):
            raise ValueError("aapt2 returned malformed packaged native ABIs")
        abis += re.findall(r"'([A-Za-z0-9_-]+)'", values)
        if len(abis) != len(set(abis)):
            raise ValueError("aapt2 repeated a packaged native ABI")
    minimum, legacy_minimum = sdk("minSdkVersion"), sdk("sdkVersion")
    if minimum is not None and legacy_minimum is not None and minimum != legacy_minimum:
        raise ValueError("aapt2 returned conflicting minimum SDK values")
    return {"package": package[1], "version_code": None if low is None else (high << 32) | low,
            "min_sdk": minimum if minimum is not None else legacy_minimum, "target_sdk": sdk("targetSdkVersion"),
            "packaged_native_abis": sorted(abis)}


def parse_manifest(text: str) -> dict:
    # Current AAPT2 badging omits versionCodeMajor and omitted SDK attributes.
    # Inspect the actual manifest to distinguish absence (Android defaults)
    # from a missing/changed badging record. Only direct uses-sdk children count.
    stack = []
    root_seen, sdk_seen = False, False
    version, sdk = {}, {}
    attributes = {"versionCode": "0101021b", "versionCodeMajor": "01010576",
                  "minSdkVersion": "0101020c", "targetSdkVersion": "01010270"}
    for line in text.splitlines():
        depth = len(line) - len(line.lstrip())
        element = re.fullmatch(r"\s*E: (\S+)(?: \(line=[0-9]+\))?", line)
        if line.lstrip().startswith("E:") and not element:
            raise ValueError("aapt2 returned malformed manifest elements")
        if element:
            while stack and depth <= stack[-1][0]:
                stack.pop()
            if not stack:
                if root_seen or element[1] != "manifest":
                    raise ValueError("aapt2 returned an ambiguous manifest root")
                root_seen = True
            elif len(stack) == 1 and element[1] == "uses-sdk":
                if sdk_seen:
                    raise ValueError("aapt2 returned multiple uses-sdk elements")
                sdk_seen = True
            stack.append((depth, element[1]))
            continue
        if not stack or depth != stack[-1][0] + 2:
            continue
        names = ("versionCode", "versionCodeMajor") if len(stack) == 1 else (
            ("minSdkVersion", "targetSdkVersion") if len(stack) == 2 and stack[-1][1] == "uses-sdk" else ())
        prefix = "A: http://schemas.android.com/apk/res/android:"
        if not any(re.match(re.escape(prefix + name) + r"(?=[(=\s]|$)", line.lstrip()) for name in names):
            continue
        match = re.fullmatch(r"\s*" + re.escape(prefix) + r"(\w+)\(0x([0-9a-fA-F]+)\)=(.*)", line)
        if not match:
            raise ValueError("aapt2 returned malformed manifest attributes")
        name, resource, raw = match.groups()
        values = version if len(stack) == 1 else sdk
        if name in values or resource.lower() != attributes[name]:
            raise ValueError("aapt2 returned ambiguous manifest attributes")
        if re.fullmatch(r"(?:[0-9]+|0x[0-9a-fA-F]+)", raw):
            values[name] = int(raw, 16 if raw.startswith("0x") else 10)
            if values is sdk and values[name] < 1:
                raise ValueError("manifest SDK versions must be positive")
        elif values is sdk:
            codename = re.fullmatch(r'"([A-Za-z][A-Za-z0-9_.]*)"(?: \(Raw: "\1"\))?', raw)
            if not codename:
                raise ValueError("manifest SDK versions must be integers or platform codenames")
            values[name] = codename[1]
        else:
            raise ValueError("manifest version attributes must be typed nonnegative integers")
    if not root_seen:
        raise ValueError("aapt2 returned no manifest root")
    low, high = version.get("versionCode", 0), version.get("versionCodeMajor", 0)
    if low > 0xffffffff or high > 0x7fffffff:
        raise ValueError("manifest version is outside the Android long-version range")
    minimum = sdk.get("minSdkVersion")
    if minimum is None:
        # Android propagates a target preview codename into an unassigned
        # minimum. Keep the codename; resolving it requires a platform profile.
        minimum = sdk.get("targetSdkVersion") if isinstance(sdk.get("targetSdkVersion"), str) else 1
    return {"version_code": (high << 32) | low,
            "declared_version_code": version.get("versionCode"),
            "declared_min_sdk": sdk.get("minSdkVersion"), "declared_target_sdk": sdk.get("targetSdkVersion"),
            "min_sdk": minimum, "target_sdk": sdk.get("targetSdkVersion", minimum)}


def parse_manifest_version(text: str) -> int:
    return parse_manifest(text)["version_code"]


def parse_certificates(text: str) -> dict:
    certificates = {}
    source_stamp = None
    for line in text.splitlines():
        # Match the metric at the start of a signer record, not text embedded
        # in a DN or another diagnostic. Certificate subjects are arbitrary.
        if not re.match(r"^(?:Signer #\S+|Source Stamp Signer) certificate SHA-256(?:\s|:|$)", line):
            if re.match(r"^[^:]+ certificate SHA-256 digest:", line):
                raise ValueError("apksigner returned an unknown certificate role")
            continue
        stamp = re.fullmatch(r"Source Stamp Signer certificate SHA-256 digest: ([0-9a-fA-F]{64})", line)
        if stamp:
            if source_stamp is not None:
                raise ValueError("apksigner repeated its source stamp certificate")
            source_stamp = stamp[1].lower()
            continue
        match = re.fullmatch(r"Signer #([1-9][0-9]*) certificate SHA-256 digest: ([0-9a-fA-F]{64})", line)
        if not match or int(match[1]) in certificates:
            raise ValueError("apksigner returned an ambiguous certificate identity")
        certificates[int(match[1])] = match[2].lower()
    if not certificates or set(certificates) != set(range(1, len(certificates) + 1)):
        raise ValueError("apksigner did not return a complete signer set")
    return {"signer_certificate_sha256": [certificates[index] for index in sorted(certificates)],
            "source_stamp_certificate_sha256": source_stamp}


def parse_verified_signers(text: str) -> dict:
    """Validate the SDK-API helper's machine output, without deriving signer roles."""
    result = json.loads(text)
    if not isinstance(result, dict) or set(result) != {
            "signer_certificate_sha256", "source_stamp_certificate_sha256", "signer_api_ranges"}:
        raise ValueError("SDK signer helper returned an unexpected schema")
    signers, stamp, ranges = (result[key] for key in (
        "signer_certificate_sha256", "source_stamp_certificate_sha256", "signer_api_ranges"))
    if (not isinstance(signers, list) or not signers
            or any(not isinstance(value, str) or not HASH.fullmatch(value) for value in signers)
            or len(set(signers)) != len(signers)):
        raise ValueError("SDK signer helper returned an invalid current signer set")
    if stamp is not None and (not isinstance(stamp, str) or not HASH.fullmatch(stamp)):
        raise ValueError("SDK signer helper returned an invalid source stamp")
    if not isinstance(ranges, list):
        raise ValueError("SDK signer helper returned invalid API ranges")
    for item in ranges:
        if (not isinstance(item, dict) or set(item) != {
                "scheme", "min_sdk", "max_sdk", "rotation_targets_dev_release", "certificate_sha256"}
                or item["scheme"] not in ("v3", "v3.1")
                or type(item["min_sdk"]) is not int or type(item["max_sdk"]) is not int
                or not 0 <= item["min_sdk"] <= item["max_sdk"] <= 0x7fffffff
                or type(item["rotation_targets_dev_release"]) is not bool
                or not isinstance(item["certificate_sha256"], str)
                or not HASH.fullmatch(item["certificate_sha256"])):
            raise ValueError("SDK signer helper returned an invalid API range")
    return result


def collect(apk: Path, expected_package: str, expected_version_code: int,
            expected_sha256: str, tools: Path, java: str = "java") -> dict:
    if not HASH.fullmatch(expected_sha256):
        raise ValueError("a pinned APK SHA-256 is required for identity collection")
    if type(expected_version_code) is not int or expected_version_code < 0:
        raise ValueError("expected versionCode must be a nonnegative integer")
    aapt = tools / ("aapt2.exe" if os.name == "nt" else "aapt2")
    signer = tools / "lib/apksigner.jar"
    signer_helper = Path(__file__).with_name("SdkApkSigners.java")
    java_executable = shutil.which(java)
    if not java_executable:
        raise ValueError("Java is required to verify APK signatures")
    tool_identity = {"build_tools_directory_name": tools.name,
                     "aapt2_sha256": hash_file(aapt), "apksigner_jar_sha256": hash_file(signer),
                     "java_executable_sha256": hash_file(Path(java_executable)),
                     "signer_helper_sha256": hash_file(signer_helper)}
    with tempfile.TemporaryDirectory(prefix="gesher-apk-identity-") as directory:
        snapshot = Path(directory) / "pinned.apk"
        shutil.copyfile(apk, snapshot)
        measured = hash_file(snapshot)
        if measured != expected_sha256.lower():
            raise ValueError("APK bytes do not match the pinned SHA-256")
        metadata = parse_badging(run_tool([str(aapt), "dump", "badging", str(snapshot)], "aapt2"))
        manifest = parse_manifest(run_tool([
            str(aapt), "dump", "xmltree", "--file", "AndroidManifest.xml", str(snapshot)], "aapt2 manifest"))
        if metadata["version_code"] is None:
            # AAPT2 prints the empty form for both omission and explicit zero.
            # The manifest establishes which case occurred; never default a
            # contradictory nonzero declaration to zero.
            if manifest["declared_version_code"] not in (None, 0):
                raise ValueError("aapt2 badging omitted a nonzero manifest versionCode")
        elif (metadata["version_code"] & 0xffffffff) != (manifest["version_code"] & 0xffffffff):
            raise ValueError("aapt2 badging and manifest versionCode disagree")
        for field in ("min_sdk", "target_sdk"):
            declared = manifest["declared_" + field]
            # Some SDK versions print defaults in badging. Absence is accepted
            # only when xmltree establishes that no explicit attribute exists.
            if (declared is not None and metadata[field] != declared) or (
                    metadata[field] is not None and metadata[field] != manifest[field]):
                raise ValueError(f"aapt2 badging and manifest {field} disagree")
        metadata.update(manifest)
        if metadata["package"] != expected_package or metadata["version_code"] != expected_version_code:
            raise ValueError("APK package/versionCode does not match the manifest entry")
        certificate_output = run_tool([
            java_executable, "-jar", str(signer), "verify", "--print-certs", str(snapshot)], "apksigner")
        if re.search(r"^Signer \(", certificate_output, re.MULTILINE):
            # v3.1 CLI output lists API-range signers without identifying the
            # current set. Obtain that set from the same SDK verifier's result;
            # neither output order nor the largest SDK range defines identity.
            certificates = parse_verified_signers(run_tool([
                java_executable, "--class-path", str(signer), str(signer_helper), str(snapshot)],
                "SDK verified signer identity (JDK 17+ required)"))
        else:
            certificates = {**parse_certificates(certificate_output), "signer_api_ranges": []}
        if hash_file(snapshot) != measured:
            raise ValueError("APK snapshot changed during identity collection")
    return {"schema_version": 1, "source": "android_sdk_build_tools", "apk_sha256": measured,
            **metadata, **certificates,
            "signature_verification": "apksigner_default_supported_platforms", "tools": tool_identity}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--version-code", type=int, required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--build-tools", type=Path)
    parser.add_argument("--java", default="java")
    args = parser.parse_args()
    try:
        result = collect(args.apk, args.package, args.version_code, args.sha256,
                         build_tools(args.build_tools), args.java)
    except (OSError, ValueError) as error:
        print(f"APK identity unavailable: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
