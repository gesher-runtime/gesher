#!/usr/bin/env python3
"""Run SDK36 WifiInfo callers over freshly built production value classes.

The host adapter supplies native initialization/logging and a real elapsed clock.
It does not implement Parcel, Wi-Fi services, or the value contracts. The same
Java fixture requires native Parcel in the Windows ART gate. --source-ref replaces
the old WifiInfo after compiling its dependencies; a strict loader exposes the
missing SDK Builder or incorrect redaction mask without cached-provider fallback.
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

ROOT = Path(__file__).resolve().parents[1]
FAMILY = ("wifi/WifiInfo", "wifi/WifiConfiguration", "wifi/WifiEnterpriseConfig",
          "wifi/SecurityParams", "wifi/ParcelUtil", "wifi/OuiKeyedData",
          "wifi/util/PersistableBundleUtils", "wifi/WifiSsid", "wifi/util/HexEncoding",
          "wifi/SupplicantState", "wifi/MloLink", "wifi/ScanResult", "wifi/ScanResultSecurity",
          "wifi/AnqpInformationElement", "MacAddress", "TransportInfo")


def check_declarations(sdk_jar: Path, classes: Path) -> dict:
    result = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name in ("WifiInfo", "WifiInfo$Builder"):
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
        info = members.class_members((classes / "android/net/wifi/WifiInfo.class").read_bytes())
        bridge = [m for m in info.members.values() if m.name == "makeCopy"
                  and m.descriptor == "(J)Landroid/net/TransportInfo;"]
        if len(bridge) != 1 or bridge[0].flags & 0x1041 != 0x1041:
            raise AssertionError("Missing public synthetic TransportInfo copy bridge")
    return {"scope": "Selected SDK36 declarations from actual compiled production output; no runtime percentage",
            "sdk_sha256": hashlib.sha256(sdk_jar.read_bytes()).hexdigest(), "classes": result,
            "transport_copy_bridge_flags": bridge[0].flags}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref", help="Compile the old WifiInfo as a negative control")
    parser.add_argument("--output-dir", type=Path, help="Retain evidence in a new, empty directory")
    parser.add_argument("--negative-mode", choices=("values", "linkage"), default="values")
    parser.add_argument("--inventory-output", type=Path, help="Write actual candidate declaration evidence")
    args = parser.parse_args()
    if args.source_ref and args.inventory_output:
        parser.error("An intentionally replaced baseline cannot produce candidate declaration evidence")
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
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-wifi-info-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir()
        callers.mkdir()
        library = work / "libwifi_info_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home.resolve() / "include"),
                        "-I" + str(args.java_home.resolve() / "include/linux"),
                        str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(library)],
                       check=True)
        # No framework/app classes on this compilation path: the SDK binds every public call site.
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp",
                        str(args.sdk_jar.resolve()), "-d", str(callers),
                        str(ROOT / "tests/dex-services/gesher/services/WifiInfoContractTest.java")],
                       check=True)
        sources = [str(ROOT / "app/java/android/net" / (name + ".java")) for name in FAMILY]
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        if not args.source_ref:
            inventory = check_declarations(args.sdk_jar, classes)
            if args.inventory_output:
                args.inventory_output.write_text(json.dumps(inventory, indent=2) + "\n")
            print("Selected SDK36 WifiInfo declarations:",
                  {name: len(item["declarations"]) for name, item in inventory["classes"].items()}, flush=True)
        if args.source_ref:
            relative = "app/java/android/net/wifi/WifiInfo.java"
            source = work / "old-source" / "WifiInfo.java"
            source.parent.mkdir()
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                              cwd=ROOT, capture_output=True, check=True).stdout)
            for stale in (classes / "android/net/wifi").glob("WifiInfo*.class"):
                stale.unlink()
            subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp",
                            os.pathsep.join((str(classes), classpath)), "-d", str(classes),
                            str(source)], check=True)
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", str(callers),
                        "-d", str(callers), str(ROOT / "tests/wifi-info/WifiInfoHost.java")],
                       check=True)
        subprocess.run([java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions",
                        "-XX:-BytecodeVerificationRemote", "-cp",
                        os.pathsep.join((str(classes), str(callers), classpath)),
                        "gesher.services.WifiInfoHost", str(library), str(classes),
                        str(args.framework_classes.resolve()), args.negative_mode if args.source_ref else "candidate",
                        str(callers)],
                       check=True, timeout=90)
        print("Public SDK36 callers; actual production WifiInfo providers; "
              "real elapsed clock and framework IPv4 conversion. Native Parcel is a separate ART gate.")


if __name__ == "__main__":
    main()
