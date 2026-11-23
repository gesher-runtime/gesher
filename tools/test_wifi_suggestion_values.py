#!/usr/bin/env python3
"""SDK36 Suggestion/Passpoint value contracts over freshly compiled production providers.

The host boundary performs framework native initialization; it provides neither
Parcel nor Wi-Fi behavior. Native wire validation is a required Windows ART gate.
The negative control proves all eight selected files are absent at the named source
revision and runs the same SDK callers with strict, non-fallback class loading.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

import java_member_inventory as members
from test_wifi_info_contracts import FAMILY as WIFI_FAMILY

ROOT = Path(__file__).resolve().parents[1]
NEW_FAMILY = ("wifi/WifiNetworkSuggestion", "wifi/hotspot2/PasspointConfiguration", "wifi/hotspot2/pps/HomeSp",
              "wifi/hotspot2/pps/Credential", "wifi/hotspot2/pps/Policy", "wifi/hotspot2/pps/UpdateParameter",
              "wifi/EAPConstants", "wifi/util/BuildProperties")
FAMILY = (*NEW_FAMILY, *WIFI_FAMILY)
SELECTED = ("WifiNetworkSuggestion", "WifiNetworkSuggestion$Builder", "hotspot2/PasspointConfiguration",
            "hotspot2/pps/HomeSp", "hotspot2/pps/Credential", "hotspot2/pps/Credential$UserCredential",
            "hotspot2/pps/Credential$CertificateCredential", "hotspot2/pps/Credential$SimCredential")
FIXTURES = ("WifiSuggestionValuesContractTest.java", "WifiSuggestionCertificates.java")


def check_declarations(sdk_jar: Path, classes: Path) -> dict:
    result = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name in SELECTED:
            relative = "android/net/wifi/" + name + ".class"
            expected = members.class_members(sdk.read(relative))
            raw = (classes / relative).read_bytes()
            actual = members.class_members(raw)
            if not actual.flags & members.PUBLIC or (actual.flags & 0x6610) != (expected.flags & 0x6610):
                raise AssertionError("SDK class access/kind mismatch: " + name)
            if actual.superclass != expected.superclass or not set(expected.interfaces) <= set(actual.interfaces):
                raise AssertionError("SDK class ancestry mismatch: " + name)
            rows = []
            for member in sorted(expected.members.values()):
                if not member.flags & (members.PUBLIC | members.PROTECTED):
                    continue
                provided = actual.members.get(member.key)
                if provided is None or members.flag_issues(member, provided):
                    raise AssertionError(f"SDK declaration mismatch {name}: {member.key}")
                rows.append({"kind": member.kind, "name": member.name, "descriptor": member.descriptor,
                             "sdk_flags": member.flags, "candidate_flags": provided.flags})
            result[expected.name] = {"declarations": rows, "compiled_sha256": hashlib.sha256(raw).hexdigest()}
    counts = [len(result["android.net.wifi." + name.replace("/", ".")]["declarations"]) for name in SELECTED]
    if counts != [30, 31, 18, 20, 24, 16, 12, 12]:
        raise AssertionError("Selected SDK profile changed: " + str(counts))
    return {"scope": "Selected SDK36 public/protected declarations; no runtime coverage percentage",
            "sdk_sha256": hashlib.sha256(sdk_jar.read_bytes()).hexdigest(), "classes": result,
            "total_declarations": sum(counts),
            "constant_values": "2 runtime reflection checks against SDK-inlined constants in the same corpus"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref", help="Exact baseline commit where all selected production files are absent")
    parser.add_argument("--output-dir", type=Path, help="Retain evidence in a new empty directory")
    parser.add_argument("--inventory-output", type=Path)
    args = parser.parse_args()
    if args.source_ref and args.inventory_output:
        parser.error("A negative control cannot produce candidate declaration evidence")
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the declared initialization boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve()
            work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-wifi-suggestion-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir()
        callers.mkdir()
        library = work / "libwifi_suggestion_values_host.so"
        commands = []

        def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
            commands.append(command)
            (work / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
            return subprocess.run(command, cwd=ROOT, **kwargs)

        run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
             "-I" + str(args.java_home.resolve() / "include"),
             "-I" + str(args.java_home.resolve() / "include/linux"),
             str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(library)], check=True)
        # No production or framework output can supply a public call-site declaration.
        run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", str(args.sdk_jar.resolve()),
             "-d", str(callers), *(str(ROOT / "tests/dex-services/gesher/services" / name) for name in FIXTURES)],
            check=True)
        family = FAMILY
        if args.source_ref:
            revision = subprocess.check_output(["git", "rev-parse", args.source_ref + "^{commit}"],
                                               cwd=ROOT, text=True).strip()
            for name in NEW_FAMILY:
                item = "app/java/android/net/" + name + ".java"
                probe = subprocess.run(["git", "ls-tree", "--full-tree", "--name-only", revision, "--", item],
                                       cwd=ROOT, capture_output=True, check=True)
                if probe.stdout:
                    raise AssertionError("Baseline source absence is not proven: " + item)
            family = WIFI_FAMILY
        run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", classpath, "-d", str(classes),
             *(str(ROOT / "app/java/android/net" / (name + ".java")) for name in family)], check=True)
        if not args.source_ref:
            inventory = check_declarations(args.sdk_jar, classes)
            if args.inventory_output:
                args.inventory_output.write_text(json.dumps(inventory, indent=2) + "\n")
            print("Selected SDK36 Suggestion/Passpoint declarations: 61 + 102 = 163", flush=True)
        run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", str(callers),
             "-d", str(callers), str(ROOT / "tests/wifi-suggestion/WifiSuggestionValuesHost.java")], check=True)
        command = [java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                   "-cp", os.pathsep.join((str(classes), str(callers), classpath)), "gesher.services.WifiSuggestionValuesHost",
                   str(library), str(classes), str(callers)]
        if args.source_ref:
            controls = []
            for mode, missing in (("suggestion", "android.net.wifi.WifiNetworkSuggestion$Builder"),
                                  ("passpoint", "android.net.wifi.hotspot2.PasspointConfiguration")):
                result = run([*command, mode], capture_output=True, text=True, timeout=90)
                output = result.stdout + result.stderr
                (work / (mode + "-negative.log")).write_text(output)
                if result.returncode == 0 or ("ClassNotFoundException: " + missing) not in output:
                    raise AssertionError("Negative control did not fail at the selected absent provider:\n" + output)
                controls.append({"mode": mode, "missing": missing, "exit_code": result.returncode})
            (work / "negative-controls.json").write_text(json.dumps(
                {"baseline_revision": revision, "controls": controls,
                 "provider_policy": "Source absent; strict findClass forbids compilation-stub fallback"}, indent=2) + "\n")
            print("Both exact-source absence controls failed at their selected SDK linkage as expected.", flush=True)
        else:
            run([*command, "candidate"], check=True, timeout=90)
            print("SDK-only callers; fresh production value graph. Native Parcel remains a separate required ART gate.")


if __name__ == "__main__":
    main()
