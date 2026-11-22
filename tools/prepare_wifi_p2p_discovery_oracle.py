#!/usr/bin/env python3
"""Freeze ordinary discovery-value profiles for independently reviewed root execution.

Local compilation and request files only: never executes or queues device actions.
Each claimed record requires its complete original reader and identical rewrite.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import time

import java_member_inventory as members
from audit_wifi_p2p_discovery_sources import (HELPERS, MODULES_UTILS_COMMIT, ROOT, SOURCE_DIR,
        SUPPORT, UPSTREAM, WIFI_COMMIT, audit, sha)
from prepare_wifi_p2p_oracle import jar
from test_wifi_p2p_discovery_values import (EXTENDED, PACKAGE, PUBLIC, SDK_SHA256, SELECTED,
        check_declarations, graph_sources, write_json)

CASE_NAMES = ("discovery_full", "discovery_social", "discovery_single", "discovery_full_frequency",
    "advertisement_default", "advertisement_explicit", "usd_discovery_default", "usd_discovery_multiple",
    "usd_discovery_maximum", "dir_zero", "dir_bytes", "dir_aliased", "discovery_vendor_one",
    "discovery_vendor_duplicate_oui", "response_null", "response_empty", "response_bytes")
LEGACY_CASE_NAMES = (*CASE_NAMES[:4], *CASE_NAMES[12:14])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes", "d8-jar",
                 "upstream-dir", "support-dir", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if sha(args.sdk_jar) != SDK_SHA256:
        raise AssertionError("Pinned official SDK36r02 required")
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    dirs = {name: out / name for name in ("candidate-classes", "pinned-r4-classes", "sdk-callers",
        "extended-classes", "launcher-classes", "source-snapshot")}
    for path in dirs.values(): path.mkdir()
    commands = []

    def run(argv: list[str]):
        started = time.monotonic()
        result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=180)
        log = f"build-step-{len(commands) + 1:02d}.log"
        (out / log).write_text(result.stdout + result.stderr, encoding="utf-8", newline="\n")
        commands.append({"argv": argv, "exit_code": result.returncode, "seconds": time.monotonic() - started, "output": log})
        write_json(out / "commands.json", commands)
        if result.returncode:
            raise RuntimeError(f"Capsule build step {len(commands)} failed:\n{result.stdout}{result.stderr}")
        return result

    javac, java = (str(args.java_home.resolve() / "bin" / name) for name in ("javac", "java"))
    common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
    cp = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    launcher = ROOT / "tests/wifi-p2p-discovery/WifiP2pDiscoveryValuesOracle.java"
    write_json(out / "source-token-audit.json", audit(args.upstream_dir, args.support_dir))
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["sdk-callers"]), str(PUBLIC)])
    provenance = {}
    for profile in ("candidate", "pinned-r4"):
        sources, manager, provenance[profile] = graph_sources(out, profile, args.upstream_dir, args.support_dir, args.sdk_jar)
        compile_cp = os.pathsep.join((str(manager), cp)) if profile == "pinned-r4" else cp
        run([*common, "-cp", compile_cp, "-d", str(dirs[profile + "-classes"]), *map(str, sources)])
        write_json(out / (profile + "-sdk-members.json"), check_declarations(args.sdk_jar, dirs[profile + "-classes"]))
    run([*common, "-cp", os.pathsep.join((str(dirs["candidate-classes"]), str(dirs["sdk-callers"]), cp)),
        "-d", str(dirs["extended-classes"]), str(EXTENDED)])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["launcher-classes"]), str(launcher)])

    def entries(directory: Path) -> dict[str, Path]:
        return {p.relative_to(directory).as_posix(): p for p in directory.rglob("*.class")}

    callers = {**entries(dirs["sdk-callers"]), **entries(dirs["extended-classes"])}
    payloads = {"stock": callers, "candidate": {**entries(dirs["candidate-classes"]), **callers},
        "pinned-r4": {**entries(dirs["pinned-r4-classes"]), **callers}, "launcher": entries(dirs["launcher-classes"])}
    value_prefixes = tuple((PACKAGE + name).replace("/", ".") for name in SELECTED)
    helper_prefixes = ("android.net.wifi.OuiKeyedData", "android.net.wifi.util.PersistableBundleUtils",
                       "android.internal.modules.utils.build.SdkLevel")
    d8_access_markers = {name + "-IA" for name in (helper_prefixes[0], value_prefixes[0], value_prefixes[2], value_prefixes[4])}

    def owned(name: str) -> bool:
        return name.startswith("gesher.services.WifiP2pDiscoveryValues") or any(
            name == prefix or name.startswith(prefix + "$") for prefix in (*value_prefixes, *helper_prefixes))

    dex_manifests = {}
    for profile, payload in payloads.items():
        for path in payload:
            name = path.removesuffix(".class").replace("/", ".")
            if not owned(name):
                raise AssertionError("Unlisted classfile provider: " + path)
            if profile in ("stock", "launcher") and not name.startswith("gesher.services.WifiP2pDiscoveryValues"):
                raise AssertionError("Stock/launcher must contain fixture classes only")
        jar(out / (profile + "-classes.jar"), payload)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8", "--min-api", "36",
             "--lib", str(args.sdk_jar.resolve()), "--output", str(out / (profile + ".jar")), str(out / (profile + "-classes.jar"))])
        definitions, manifest = members.archive_members(out / (profile + ".jar"), "boot", profile)
        if any(not owned(name) and name not in d8_access_markers for name in definitions):
            raise AssertionError("Unlisted DEX provider: " + profile)
        markers = {name for name in definitions if name in d8_access_markers}
        if markers != (d8_access_markers if profile in ("candidate", "pinned-r4") else set()):
            raise AssertionError("D8 private-constructor marker census changed: " + profile)
        for name in markers:
            marker = definitions[name]
            if marker.flags != 0x1011 or marker.superclass != "java.lang.Object" or marker.interfaces or marker.members:
                raise AssertionError("D8 access marker is no longer an empty public final synthetic class: " + name)
        if profile in ("stock", "launcher") and any(not name.startswith("gesher.services.WifiP2pDiscoveryValues") for name in definitions):
            raise AssertionError("Source implementation leaked into stock/launcher DEX")
        dex_manifests[profile] = {"manifest": manifest, "defined_classes": sorted(definitions)}

    tracked = [PUBLIC, EXTENDED, launcher, ROOT / "tests/wifi-p2p-discovery/WifiP2pDiscoveryValuesHost.java",
        ROOT / "app/java/android/net/wifi/ScanResult.java",
        *(SOURCE_DIR / relative for relative in UPSTREAM), *(ROOT / "app/java" / relative for relative in HELPERS),
        *(ROOT / relative for relative in ("tools/prepare_wifi_p2p_discovery_oracle.py",
            "tools/audit_wifi_p2p_discovery_sources.py", "tools/test_wifi_p2p_discovery_values.py",
            "tools/audit_wifi_p2p_sources.py", "tools/prepare_wifi_p2p_oracle.py", "tools/test_wifi_p2p_values.py",
            "tools/java_member_inventory.py", "tools/java_api_inventory.py"))]
    for source in tracked:
        target = dirs["source-snapshot"] / "candidate" / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(source.read_bytes())
    for category, source_dir, expected in (("upstream", args.upstream_dir, UPSTREAM), ("support", args.support_dir, SUPPORT)):
        for relative, value in expected.items():
            raw = source_dir / relative
            if sha(raw) != value: raise AssertionError("Pinned source changed during build: " + relative)
            target = dirs["source-snapshot"] / category / relative
            target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(raw.read_bytes())
    manifest = {"source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_files": {str(p.relative_to(ROOT)): sha(p) for p in tracked}, "wifi_commit": WIFI_COMMIT,
        "modules_utils_commit": MODULES_UTILS_COMMIT, "source_provenance": provenance,
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar), "app_compile_jar_sha256": sha(args.app_classes),
        "compiler_sha256": {name: sha(args.java_home / "bin" / name) for name in ("java", "javac")},
        "framework_compile_class_sha256": {name: sha(args.framework_classes / name) for name in (
            "android/os/Parcel.class", "android/os/Parcelable.class", "android/os/PersistableBundle.class",
            "android/os/Build.class", "android/os/Build$VERSION.class",
            "android/internal/modules/utils/build/SdkLevel.class")},
        "public_callers": "official SDK36 only", "extended_callers": "source-level hidden vendor methods and response constructor",
        "owned_graph": "five values, nested builders/original readers, complete OuiKeyedData/PersistableBundleUtils/SdkLevel",
        "genuine_boot_leaves": ["Parcel", "Parcelable", "PersistableBundle", "Build", "MacAddress"],
        "production_helper_definition_added": False, "sdk_or_manager_definition_packaged": False,
        "cases": {"candidate": list(CASE_NAMES), "pinned-r4": list(CASE_NAMES), "stock_if_complete": list(CASE_NAMES),
            "stock_if_modern_types_absent": list(LEGACY_CASE_NAMES), "writer_only": []},
        "stock_profile": "actual boot types recorded; absence qualifies the six common DiscoveryConfig records only",
        "native_observations": "pending root-owned execution after independent preflight",
        "payload_classfiles": {name: {item: sha(path) for item, path in sorted(payload.items())} for name, payload in payloads.items()},
        "dex_providers": dex_manifests,
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(out.glob("*.jar"))}}
    write_json(out / "manifest.json", manifest)
    remote = "/data/local/tmp/gesher-p2p-discovery-" + sha(out / "candidate.jar")[:12]
    setup = {"description": "Install frozen ordinary P2P discovery value graphs", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 60} for name in payloads),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30}]}
    write_json(out / "setup-request.json", setup)
    for profile in ("stock", "pinned-r4", "candidate"):
        request = {"description": "P2P discovery " + profile + " ordinary records with complete original readers", "steps": [
            {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                     "gesher.services.WifiP2pDiscoveryValuesOracle", profile, remote + "/" + profile + ".jar"], "timeout": 120}]}
        write_json(out / (profile + "-request.json"), request)
    write_json(out / "cleanup-request.json", {"description": "Remove only owned frozen discovery value capsule files", "steps": [
        {"adb": ["shell", "rm", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "rmdir", remote], "timeout": 30}]})
    print(json.dumps({"output": str(out), "remote": remote, "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
