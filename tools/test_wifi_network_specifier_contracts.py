#!/usr/bin/env python3
"""Run SDK36 request callers against freshly compiled genuine Wi-Fi value providers.

Host JNI supplies only framework initialization, logging and the real elapsed clock.
Native Parcel and Android boot-loader dispatch remain separate ART gates.
--baseline-control removes all three absent pre-change classes and uses a strict
loader, so cached framework API stubs cannot fill the missing runtime provider.
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
from test_wifi_info_contracts import FAMILY as WIFI_VALUES, ROOT

FAMILY = (*WIFI_VALUES, "wifi/WifiNetworkSpecifier", "wifi/WifiNetworkAgentSpecifier")
FIXTURES = ("WifiNetworkSpecifierContractTest.java", "WifiSpecifierCertificates.java")


def check_declarations(sdk_jar: Path, classes: Path) -> dict:
    result = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name in ("WifiNetworkSpecifier", "WifiNetworkSpecifier$Builder"):
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
    return {"scope": "Selected SDK36 declarations on freshly compiled production classes; no runtime percentage",
            "sdk_sha256": hashlib.sha256(sdk_jar.read_bytes()).hexdigest(), "classes": result}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--baseline-control", action="store_true")
    parser.add_argument("--output-dir", type=Path, help="Retain evidence in a new directory")
    parser.add_argument("--inventory-output", type=Path)
    args = parser.parse_args()
    if args.baseline_control and args.inventory_output:
        parser.error("A missing-provider control cannot produce candidate declaration evidence")
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the declared initialization boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve(); work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-wifi-specifier-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir(); callers.mkdir()
        library = work / "libwifi_specifier_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home.resolve() / "include"),
                        "-I" + str(args.java_home.resolve() / "include/linux"),
                        str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(library)], check=True)
        # Only the official public SDK binds caller sites; hidden state uses reflection.
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp",
                        str(args.sdk_jar.resolve()), "-d", str(callers),
                        *(str(ROOT / "tests/dex-services/gesher/services" / name) for name in FIXTURES)], check=True)
        sources = [str(ROOT / "app/java/android/net" / (name + ".java")) for name in FAMILY]
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        if args.baseline_control:
            for name in ("WifiNetworkSpecifier", "WifiNetworkAgentSpecifier"):
                for path in (classes / "android/net/wifi").glob(name + "*.class"):
                    path.unlink()
        else:
            inventory = check_declarations(args.sdk_jar, classes)
            if args.inventory_output:
                args.inventory_output.write_text(json.dumps(inventory, indent=2) + "\n")
            print("Selected SDK36 WifiNetworkSpecifier declarations:",
                  {name: len(item["declarations"]) for name, item in inventory["classes"].items()}, flush=True)
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", str(callers),
                        "-d", str(callers), str(ROOT / "tests/wifi-network-specifier/WifiNetworkSpecifierHost.java")], check=True)
        subprocess.run([java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                        "-cp", os.pathsep.join((str(classes), str(callers), classpath)),
                        "gesher.services.WifiNetworkSpecifierHost", str(library), str(classes),
                        str(args.framework_classes.resolve()), "baseline" if args.baseline_control else "candidate",
                        str(callers)], check=True, timeout=90)
        print("Actual production request, matcher and value providers; public SDK36 caller sites. "
              "Native Parcel and boot-loader NetworkRequest dispatch require the ART gate.")


if __name__ == "__main__":
    main()
