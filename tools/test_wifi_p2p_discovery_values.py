#!/usr/bin/env python3
"""SDK36 callers over fresh genuine discovery values and complete dependency graphs.

This host has genuine existing framework initialization, no Parcel replacement.
Native reader qualification is a separate exact-profile Android/Windows gate.
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
from audit_wifi_p2p_discovery_sources import (HELPERS, ROOT, SOURCE_DIR, SUPPORT, UPSTREAM,
        adapt_sdklevel, adapt_value, audit, remove_build_annotations, sha)

SDK_SHA256 = "d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a"
SELECTED = ("WifiP2pDiscoveryConfig", "WifiP2pDiscoveryConfig$Builder",
    "WifiP2pUsdBasedLocalServiceAdvertisementConfig", "WifiP2pUsdBasedLocalServiceAdvertisementConfig$Builder",
    "WifiP2pUsdBasedServiceDiscoveryConfig", "WifiP2pUsdBasedServiceDiscoveryConfig$Builder",
    "WifiP2pDirInfo", "nsd/WifiP2pUsdBasedServiceResponse")
EXPECTED_COUNTS = (6, 3, 5, 3, 6, 4, 8, 6)
PACKAGE = "android/net/wifi/p2p/"
PUBLIC = ROOT / "tests/dex-services/gesher/services/WifiP2pDiscoveryValuesContractTest.java"
EXTENDED = ROOT / "tests/dex-services/gesher/services/WifiP2pDiscoveryValuesExtendedTest.java"


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


def check_declarations(sdk_jar: Path, classes: Path) -> dict:
    if sha(sdk_jar) != SDK_SHA256:
        raise AssertionError("Pinned official SDK36r02 required")
    rows = {}
    with zipfile.ZipFile(sdk_jar) as sdk:
        for name, count in zip(SELECTED, EXPECTED_COUNTS):
            path = PACKAGE + name + ".class"
            expected = members.class_members(sdk.read(path))
            actual = members.class_members((classes / path).read_bytes())
            if not actual.flags & members.PUBLIC or (actual.flags & 0x6610) != (expected.flags & 0x6610):
                raise AssertionError("SDK class access/kind mismatch: " + name)
            if actual.superclass != expected.superclass or not set(expected.interfaces) <= set(actual.interfaces):
                raise AssertionError("SDK ancestry mismatch: " + name)
            declarations = []
            for declaration in sorted(expected.members.values()):
                if not declaration.flags & (members.PUBLIC | members.PROTECTED):
                    continue
                provided = actual.members.get(declaration.key)
                if provided is None or members.flag_issues(declaration, provided):
                    raise AssertionError(f"SDK declaration mismatch {name}: {declaration.key}")
                declarations.append({"kind": declaration.kind, "name": declaration.name,
                    "descriptor": declaration.descriptor, "sdk_flags": declaration.flags, "candidate_flags": provided.flags})
            if len(declarations) != count:
                raise AssertionError("SDK profile changed: " + name)
            rows[expected.name] = {"declarations": declarations, "compiled_sha256": sha(classes / path)}
    return {"scope": "SDK36 declared public/protected presence, not behavior coverage", "sdk_sha256": SDK_SHA256,
        "classes": rows, "total_declarations": sum(EXPECTED_COUNTS),
        "hidden_contracts": "DiscoveryConfig vendor list and response construction tested separately"}


def graph_sources(work: Path, profile: str, upstream: Path, support: Path, sdk_jar: Path) -> tuple[list[Path], Path, dict]:
    """Fresh complete values/helpers; the reference keeps actual manager constant references."""
    sources = []
    provenance = {}
    generated = work / (profile + "-sources"); generated.mkdir()
    for relative, expected in UPSTREAM.items():
        raw = upstream / relative
        if sha(raw) != expected:
            raise AssertionError("Pinned value source changed: " + relative)
        if profile == "candidate":
            target = SOURCE_DIR / relative
            changes = {"provider": "candidate production source"}
        else:
            target = generated / relative; target.parent.mkdir(parents=True, exist_ok=True)
            adapted, changes = adapt_value(raw.read_text(encoding="utf-8"), relative, candidate=False)
            target.write_text(adapted, encoding="utf-8", newline="\n")
        sources.append(target)
        provenance[relative] = {"raw_sha256": expected, "compiled_source_sha256": sha(target), **changes}
    for relative in HELPERS:
        raw = support / "framework/java" / relative
        if sha(raw) != SUPPORT["framework/java/" + relative]:
            raise AssertionError("Pinned complete helper changed: " + relative)
        if profile == "candidate":
            target = ROOT / "app/java" / relative
            annotations = None
        else:
            adapted, annotations = remove_build_annotations(raw.read_text(encoding="utf-8"))
            target = generated / relative; target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(adapted, encoding="utf-8", newline="\n")
        sources.append(target)
        provenance[relative] = {"raw_sha256": sha(raw), "compiled_source_sha256": sha(target),
                               "removed_build_annotations": annotations, "complete_source": True}
    raw = support / "java/com/android/modules/utils/build/SdkLevel.java"
    if sha(raw) != SUPPORT["java/com/android/modules/utils/build/SdkLevel.java"]:
        raise AssertionError("Pinned complete SdkLevel changed")
    helper = generated / "android/internal/modules/utils/build/SdkLevel.java"
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_text(adapt_sdklevel(raw.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
    sources.append(helper)
    provenance["SdkLevel.java"] = {"raw_sha256": sha(raw), "compiled_source_sha256": sha(helper),
        "adaptations": ["genuine framework jarjar package", "unavailable AndroidX annotations removed"],
        "complete_source": True, "production_definition": False}
    constants = work / "compile-only-manager"
    path = "android/net/wifi/p2p/WifiP2pManager.class"
    with zipfile.ZipFile(sdk_jar) as sdk:
        target = constants / path; target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(sdk.read(path))
    provenance["compile_only_manager"] = {"source": "official SDK36 class; never packaged", "sha256": sha(constants / path),
        "purpose": "Reference original qualified constant expressions, independently bound to pinned manager source"}
    return sources, constants, provenance


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes", "upstream-dir", "support-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--profile", choices=("candidate", "pinned-r4"), default="candidate")
    parser.add_argument("--source-ref", help="Exact absence baseline for all five new value sources")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.source_ref and args.profile != "candidate":
        parser.error("Absence baseline and pinned source execution are separate")
    if sha(args.sdk_jar) != SDK_SHA256:
        parser.error("Official pinned SDK36r02 required")
    cc = shutil.which("cc")
    if not cc:
        parser.error("C compiler required for genuine framework initialization")
    cp = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    javac, java = (str(args.java_home.resolve() / "bin" / name) for name in ("javac", "java"))
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve(); work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-p2p-discovery-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir(); callers.mkdir()
        commands = []

        def run(argv: list[str], *, success: bool = True):
            started = time.monotonic()
            result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=120)
            log = f"step-{len(commands) + 1:02d}.log"
            (work / log).write_text(result.stdout + result.stderr, encoding="utf-8", newline="\n")
            commands.append({"argv": argv, "exit_code": result.returncode, "seconds": time.monotonic() - started, "output": log})
            write_json(work / "commands.json", commands)
            if result.returncode and success:
                raise RuntimeError(f"Step {len(commands)} failed:\n{result.stdout}{result.stderr}")
            return result

        native = work / "libp2p_discovery_host.so"
        run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
             "-I" + str(args.java_home.resolve() / "include"), "-I" + str(args.java_home.resolve() / "include/linux"),
             str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(native)])
        common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
        run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(callers), str(PUBLIC)])
        provenance = {}
        if args.source_ref:
            revision = subprocess.check_output(["git", "rev-parse", args.source_ref + "^{commit}"], cwd=ROOT, text=True).strip()
            for relative in UPSTREAM:
                path = str((SOURCE_DIR / relative).relative_to(ROOT))
                if subprocess.check_output(["git", "ls-tree", "--name-only", revision, "--", path], cwd=ROOT):
                    raise AssertionError("Absence not proven at exact baseline: " + path)
        else:
            write_json(work / "source-audit.json", audit(args.upstream_dir, args.support_dir))
            sources, manager, provenance = graph_sources(work, args.profile, args.upstream_dir, args.support_dir, args.sdk_jar)
            compile_cp = os.pathsep.join((str(manager), cp)) if args.profile == "pinned-r4" else cp
            run([*common, "-cp", compile_cp, "-d", str(classes), *map(str, sources)])
            write_json(work / "sdk-members.json", check_declarations(args.sdk_jar, classes))
            run([*common, "-cp", os.pathsep.join((str(classes), str(callers), cp)), "-d", str(callers), str(EXTENDED)])
        run([*common, "-cp", str(callers), "-d", str(callers), str(ROOT / "tests/wifi-p2p-discovery/WifiP2pDiscoveryValuesHost.java")])
        result = run([java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
            "-cp", os.pathsep.join((str(classes), str(callers), cp)), "gesher.services.WifiP2pDiscoveryValuesHost", str(native),
            str(classes), str(callers), str(args.framework_classes.resolve()), str(args.app_classes.resolve()),
            "baseline" if args.source_ref else args.profile], success=not args.source_ref)
        if args.source_ref:
            output = result.stdout + result.stderr
            missing = "android.net.wifi.p2p.WifiP2pDiscoveryConfig$Builder"
            if result.returncode == 0 or "ClassNotFoundException: " + missing not in output or "Stub!" in output:
                raise AssertionError("Exact absence baseline did not fail at missing genuine provider:\n" + output)
            write_json(work / "negative-control.json", {"source_revision": revision, "missing": missing,
                "exit_code": result.returncode, "compilation_stub_fallback": False})
            print("Discovery exact-source absence control: expected missing Builder; no stub fallback")
        else:
            print(result.stdout, end="")
            write_json(work / "providers.json", {"profile": args.profile, "source_provenance": provenance,
                "compiled_classes": {str(p.relative_to(classes)): sha(p) for p in sorted(classes.rglob("*.class"))},
                "public_fixture_sdk_only": True, "native_parcel_execution": False,
                "framework_parcel_sha256": sha(args.framework_classes / "android/os/Parcel.class"),
                "existing_app_provider_sha256": sha(args.app_classes) if args.app_classes.is_file() else None})
            print("All 41 SDK36 declarations match; genuine complete graphs; host supplies no Parcel implementation")


if __name__ == "__main__":
    main()
