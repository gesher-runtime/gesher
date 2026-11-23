#!/usr/bin/env python3
"""SDK36 P2P values over fresh production providers; host does not replace Parcel.

The public caller compiles against the SDK only. A separately identified source
fixture initializes fields normally supplied by the framework. Native Parcel is
qualified separately, with a boot-coherent DeviceList reader required on Windows.
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
import time
import zipfile

import java_member_inventory as members
from test_wifi_info_contracts import FAMILY as WIFI_FAMILY

ROOT = Path(__file__).resolve().parents[1]
P2P = ("WifiP2pConfig", "WifiP2pPairingBootstrappingConfig", "WifiP2pDevice",
       "WifiP2pWfdInfo", "WifiP2pDeviceList", "WifiP2pGroup", "WifiP2pInfo")
FAMILY = tuple("wifi/p2p/" + name for name in P2P) + ("wifi/WpsInfo", *WIFI_FAMILY)
SELECTED = ("WifiP2pConfig", "WifiP2pConfig$Builder", "WifiP2pPairingBootstrappingConfig",
            "WifiP2pDevice", "WifiP2pWfdInfo", "WifiP2pDeviceList", "WifiP2pGroup", "WifiP2pInfo")
EXPECTED_COUNTS = (33, 12, 11, 32, 45, 8, 22, 9)
SDK_SHA256 = "d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_declarations(sdk_jar: Path, classes: Path) -> dict:
    if sha(sdk_jar) != SDK_SHA256:
        raise AssertionError("This census requires the pinned SDK36 artifact")
    result = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name, count in zip(SELECTED, EXPECTED_COUNTS):
            path = "android/net/wifi/p2p/" + name + ".class"
            expected = members.class_members(sdk.read(path))
            actual = members.class_members((classes / path).read_bytes())
            if not actual.flags & members.PUBLIC or (actual.flags & 0x6610) != (expected.flags & 0x6610):
                raise AssertionError("SDK class access/kind mismatch: " + name)
            if actual.superclass != expected.superclass or not set(expected.interfaces) <= set(actual.interfaces):
                raise AssertionError("SDK ancestry mismatch: " + name)
            rows = []
            for declaration in sorted(expected.members.values()):
                if not declaration.flags & (members.PUBLIC | members.PROTECTED):
                    continue
                provided = actual.members.get(declaration.key)
                if provided is None or members.flag_issues(declaration, provided):
                    raise AssertionError(f"SDK member mismatch {name}: {declaration.key}")
                rows.append({"kind": declaration.kind, "name": declaration.name,
                             "descriptor": declaration.descriptor, "sdk_flags": declaration.flags,
                             "candidate_flags": provided.flags})
            if len(rows) != count:
                raise AssertionError("SDK profile changed: " + name)
            result[expected.name] = {"declarations": rows, "compiled_sha256": sha(classes / path)}
    return {"scope": "SDK36 declaration census, not a behavioral coverage percentage",
            "sdk_sha256": SDK_SHA256, "classes": result, "total_declarations": sum(EXPECTED_COUNTS),
            "runtime_constant_checks": 49,
            "generic_dependency": "Device.getVendorElements: java.util.List<ScanResult.InformationElement>"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref", help="Exact source baseline with all seven P2P source files absent")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--inventory-output", type=Path)
    args = parser.parse_args()
    if args.source_ref and args.inventory_output:
        parser.error("A baseline absence control cannot create candidate evidence")
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the explicit initialization boundary")
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve(); work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-p2p-values-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir(); callers.mkdir()
        library = work / "libp2p_values_host.so"
        commands = []

        def run(command: list[str], *, expect_success=True):
            started = time.monotonic()
            result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=150)
            text = result.stdout + result.stderr
            name = f"step-{len(commands) + 1:02d}.log"
            (work / name).write_text(text)
            commands.append({"argv": command, "exit_code": result.returncode,
                             "seconds": time.monotonic() - started, "output": name})
            (work / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
            if result.returncode and expect_success:
                raise RuntimeError(f"Step {len(commands)} failed:\n{text}")
            return result

        run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
             "-I" + str(args.java_home.resolve() / "include"),
             "-I" + str(args.java_home.resolve() / "include/linux"),
             str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(library)])
        common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
        # No production or cached framework declarations on this public-callsite path.
        fixture = ROOT / "tests/dex-services/gesher/services/WifiP2pValuesContractTest.java"
        run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(callers), str(fixture)])
        family = FAMILY
        if args.source_ref:
            revision = subprocess.check_output(["git", "rev-parse", args.source_ref + "^{commit}"], cwd=ROOT, text=True).strip()
            for name in P2P:
                path = "app/java/android/net/wifi/p2p/" + name + ".java"
                if subprocess.check_output(["git", "ls-tree", "--name-only", revision, "--", path], cwd=ROOT):
                    raise AssertionError("Baseline absence not proven: " + path)
            family = ("wifi/WpsInfo", *WIFI_FAMILY)
        run([*common, "-cp", classpath, "-d", str(classes),
             *(str(ROOT / "app/java/android/net" / (name + ".java")) for name in family)])
        if not args.source_ref:
            inventory = check_declarations(args.sdk_jar, classes)
            (work / "sdk-members.json").write_text(json.dumps(inventory, indent=2) + "\n")
            if args.inventory_output:
                args.inventory_output.write_text(json.dumps(inventory, indent=2) + "\n")
            run([*common, "-cp", os.pathsep.join((str(classes), str(callers), classpath)), "-d", str(callers),
                 str(ROOT / "tests/dex-services/gesher/services/WifiP2pValuesExtendedTest.java")])
        run([*common, "-cp", str(callers), "-d", str(callers), str(ROOT / "tests/wifi-p2p/WifiP2pValuesHost.java")])
        command = [java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                   "-cp", os.pathsep.join((str(classes), str(callers), classpath)), "gesher.services.WifiP2pValuesHost",
                   str(library), str(classes), str(callers), "baseline" if args.source_ref else "candidate"]
        result = run(command, expect_success=not args.source_ref)
        if args.source_ref:
            text = result.stdout + result.stderr
            missing = "android.net.wifi.p2p.WifiP2pConfig$Builder"
            if result.returncode == 0 or "ClassNotFoundException: " + missing not in text or "Stub!" in text:
                raise AssertionError("Baseline did not fail at its selected absent provider:\n" + text)
            (work / "negative-control.json").write_text(json.dumps({"source_revision": revision,
                "missing": missing, "exit_code": result.returncode, "compilation_stub_fallback": False}, indent=2) + "\n")
            print("P2P exact-source absence control: expected missing Builder, no stub fallback")
        else:
            print(result.stdout, end="")
            (work / "providers.json").write_text(json.dumps({"families": list(FAMILY),
                "compiled_classes": {str(p.relative_to(classes)): sha(p) for p in sorted(classes.rglob("*.class"))},
                "sources": {name: sha(ROOT / "app/java/android/net" / (name + ".java")) for name in FAMILY},
                "public_fixture_sdk_only": True, "native_parcel_execution": False}, indent=2) + "\n")
            print("All 172 SDK36 declarations match; host supplies no Parcel or Wi-Fi implementation")


if __name__ == "__main__":
    main()
