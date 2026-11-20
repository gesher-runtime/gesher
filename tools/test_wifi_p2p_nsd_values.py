#!/usr/bin/env python3
"""SDK36 callers over fresh NSD values and the genuine existing framework TXT helper.

Host supplies no Parcel. Native original readers have independent Android and
Windows gates. The absence control forbids fallback to compile-only stubs.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import zipfile

import java_member_inventory as members
from audit_wifi_p2p_nsd_sources import NAMES, ROOT, SOURCE_DIR, audit, sha

SDK_SHA256 = "d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a"
SELECTED = (*NAMES, "WifiP2pUsdBasedServiceConfig$Builder")
EXPECTED_COUNTS = (10, 8, 1, 3, 1, 2, 11, 4)
PACKAGE = "android/net/wifi/p2p/nsd/"


def check_declarations(sdk_jar: Path, classes: Path) -> dict:
    if sha(sdk_jar) != SDK_SHA256:
        raise AssertionError("Pinned official SDK36r02 required")
    result = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name, count in zip(SELECTED, EXPECTED_COUNTS):
            path = PACKAGE + name + ".class"
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
    return {"scope": "SDK36 declared public/protected presence, not behavior coverage",
            "sdk_sha256": SDK_SHA256, "classes": result, "total_declarations": sum(EXPECTED_COUNTS),
            "hidden_contracts": "base CREATORs, protected constructors, query and ID observers additionally tested"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes", "upstream-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref", help="Exact absence baseline for all seven new source files")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--inventory-output", type=Path)
    args = parser.parse_args()
    if args.source_ref and args.inventory_output:
        parser.error("An absence control cannot create candidate evidence")
    if sha(args.sdk_jar) != SDK_SHA256:
        parser.error("Official pinned SDK36 required")
    cc = shutil.which("cc")
    if not cc:
        parser.error("C compiler required for the explicit host initialization boundary")
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes))
    javac, java = (str(args.java_home.resolve() / "bin" / name) for name in ("javac", "java"))
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve(); work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-p2p-nsd-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir(); callers.mkdir()
        commands = []

        def run(argv: list[str], *, success=True):
            started = time.monotonic()
            result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=120)
            log = f"step-{len(commands) + 1:02d}.log"
            (work / log).write_text(result.stdout + result.stderr)
            commands.append({"argv": argv, "exit_code": result.returncode,
                             "seconds": time.monotonic() - started, "output": log})
            (work / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
            if result.returncode and success:
                raise RuntimeError(f"Step {len(commands)} failed:\n{result.stdout}{result.stderr}")
            return result

        library = work / "libp2p_nsd_host.so"
        run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
             "-I" + str(args.java_home.resolve() / "include"),
             "-I" + str(args.java_home.resolve() / "include/linux"),
             str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(library)])
        common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
        public = ROOT / "tests/dex-services/gesher/services/WifiP2pNsdValuesContractTest.java"
        run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(callers), str(public)])
        if args.source_ref:
            revision = subprocess.check_output(["git", "rev-parse", args.source_ref + "^{commit}"], cwd=ROOT, text=True).strip()
            for name in NAMES:
                path = str((SOURCE_DIR / (name + ".java")).relative_to(ROOT))
                if subprocess.check_output(["git", "ls-tree", "--name-only", revision, "--", path], cwd=ROOT):
                    raise AssertionError("Absence not proven at exact baseline: " + path)
        else:
            (work / "source-audit.json").write_text(json.dumps(audit(args.upstream_dir), indent=2) + "\n")
            run([*common, "-cp", classpath, "-d", str(classes),
                 *(str(SOURCE_DIR / (name + ".java")) for name in NAMES)])
            inventory = check_declarations(args.sdk_jar, classes)
            (work / "sdk-members.json").write_text(json.dumps(inventory, indent=2) + "\n")
            if args.inventory_output:
                args.inventory_output.write_text(json.dumps(inventory, indent=2) + "\n")
            run([*common, "-cp", os.pathsep.join((str(classes), str(callers), classpath)), "-d", str(callers),
                 str(ROOT / "tests/dex-services/gesher/services/WifiP2pNsdValuesExtendedTest.java")])
        run([*common, "-cp", str(callers), "-d", str(callers), str(ROOT / "tests/wifi-p2p-nsd/WifiP2pNsdValuesHost.java")])
        result = run([java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                      "-cp", os.pathsep.join((str(classes), str(callers), classpath)),
                      "gesher.services.WifiP2pNsdValuesHost", str(library), str(classes), str(callers),
                      str(args.framework_classes.resolve()), "baseline" if args.source_ref else "candidate"],
                     success=not args.source_ref)
        if args.source_ref:
            output = result.stdout + result.stderr
            missing = "android.net.wifi.p2p.nsd.WifiP2pServiceRequest"
            if result.returncode == 0 or "ClassNotFoundException: " + missing not in output or "Stub!" in output:
                raise AssertionError("Baseline did not fail at absent genuine provider:\n" + output)
            (work / "negative-control.json").write_text(json.dumps({"source_revision": revision, "missing": missing,
                "exit_code": result.returncode, "compilation_stub_fallback": False}, indent=2) + "\n")
            print("NSD exact-source absence control: expected missing ServiceRequest; no stub fallback")
        else:
            print(result.stdout, end="")
            (work / "providers.json").write_text(json.dumps({"sources": {name: sha(SOURCE_DIR / (name + ".java")) for name in NAMES},
                "compiled_classes": {str(p.relative_to(classes)): sha(p) for p in sorted(classes.rglob("*.class"))},
                "helper": {"provider": str(args.framework_classes.resolve()), "sha256": sha(args.framework_classes / "com/android/net/module/util/DnsSdTxtRecord.class")},
                "public_fixture_sdk_only": True, "native_parcel_execution": False}, indent=2) + "\n")
            print("All 40 SDK36 declarations match; host supplies no Parcel or Wi-Fi value implementation")


if __name__ == "__main__":
    main()
