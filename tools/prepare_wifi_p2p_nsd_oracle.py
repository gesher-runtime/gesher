#!/usr/bin/env python3
"""Freeze coherent NSD value graphs and explicit root-owned Android requests.

This script only compiles local artifacts and writes reviewable request files.
It never executes or queues device commands. Every ordinary case uses its full
original reader; source-prescribed null-list normalization is recorded separately.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import java_member_inventory as members
from audit_wifi_p2p_nsd_sources import (CONNECTIVITY_COMMIT, HELPER_SHA256, NAMES, ROOT,
                                        SOURCE_DIR, UPSTREAM, WIFI_COMMIT, audit, sha)
from prepare_wifi_p2p_oracle import RELEASE, SUPPORT, jar
from test_wifi_p2p_nsd_values import SDK_SHA256, check_declarations


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes",
                 "d8-jar", "upstream-dir", "wifi-support-dir", "release-dir", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if sha(args.sdk_jar) != SDK_SHA256:
        raise AssertionError("Pinned SDK36r02 required")
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    dirs = {name: out / name for name in ("candidate-classes", "sdk-callers", "extended-classes",
            "launcher-classes", "upstream-sources", "upstream-classes", "generated-flags", "source-snapshot")}
    for path in dirs.values(): path.mkdir()
    commands = []

    def run(argv: list[str]):
        started = time.monotonic()
        result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=180)
        log = f"build-step-{len(commands) + 1:02d}.log"
        (out / log).write_text(result.stdout + result.stderr)
        commands.append({"argv": argv, "exit_code": result.returncode,
                         "seconds": time.monotonic() - started, "output": log})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"Build step {len(commands)} failed:\n{result.stdout}{result.stderr}")
        return result

    javac, java = (str(args.java_home.resolve() / "bin" / name) for name in ("javac", "java"))
    common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
    classpath = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    public = ROOT / "tests/dex-services/gesher/services/WifiP2pNsdValuesContractTest.java"
    extended = ROOT / "tests/dex-services/gesher/services/WifiP2pNsdValuesExtendedTest.java"
    launcher = ROOT / "tests/wifi-p2p-nsd/WifiP2pNsdValuesOracle.java"
    sources = [SOURCE_DIR / (name + ".java") for name in NAMES]
    helper = args.upstream_dir / "DnsSdTxtRecord.java"
    (out / "source-token-audit.json").write_text(json.dumps(audit(args.upstream_dir), indent=2) + "\n")
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["sdk-callers"]), str(public)])
    # Complete genuine helper is selected only for these isolated native profiles.
    # Production continues to use its existing framework provider, without duplication.
    run([*common, "-cp", classpath, "-d", str(dirs["candidate-classes"]), *map(str, sources), str(helper.resolve())])
    (out / "sdk-members.json").write_text(json.dumps(check_declarations(args.sdk_jar, dirs["candidate-classes"]), indent=2) + "\n")
    run([*common, "-cp", os.pathsep.join((str(dirs["candidate-classes"]), str(dirs["sdk-callers"]), classpath)),
         "-d", str(dirs["extended-classes"]), str(extended)])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["launcher-classes"]), str(launcher)])

    originals = {name + ".java": args.upstream_dir / (name + ".java") for name in NAMES}
    originals["DnsSdTxtRecord.java"] = helper
    originals["Environment.java"] = args.wifi_support_dir / "framework/java/android/net/wifi/util/Environment.java"
    expected = {name + ".java": value for name, value in UPSTREAM.items()}
    expected["DnsSdTxtRecord.java"] = HELPER_SHA256
    expected["Environment.java"] = SUPPORT["framework/java/android/net/wifi/util/Environment.java"]
    provenance = {}
    for name, source in originals.items():
        if sha(source) != expected[name]:
            raise AssertionError("Genuine pinned source changed: " + name)
        raw = source.read_text()
        text = re.sub(r"(?m)^import android\.annotation\.(?:FlaggedApi|RequiresApi);\n", "", raw)
        text, annotations = re.subn(r"(?m)^[ \t]*@(FlaggedApi|RequiresApi)\([^\n]*\)[ \t]*\n", "", text)
        target = dirs["upstream-sources"] / name; target.write_text(text)
        snapshot = dirs["source-snapshot"] / "upstream" / name; snapshot.parent.mkdir(exist_ok=True)
        snapshot.write_bytes(source.read_bytes())
        provenance[name] = {"raw_sha256": sha(source), "compiled_source_sha256": sha(target),
                           "removed_build_annotations": annotations,
                           "adaptations": ["unavailable build annotations removed"] if annotations else [],
                           "full_value_fields_and_original_readers_retained": True}
    for name, expected_sha in RELEASE.items():
        if sha(args.release_dir / name) != expected_sha:
            raise AssertionError("Pinned release profile changed: " + name)
    flags = args.wifi_support_dir / "flags/wifi_flags.aconfig"
    if sha(flags) != SUPPORT["flags/wifi_flags.aconfig"]:
        raise AssertionError("Pinned flag declarations changed")
    values = run([sys.executable, str(ROOT / "tools/gen/aconfig_values.py"), str(args.release_dir.resolve()), "bp4a"]).stdout
    if "com.android.wifi.flags.wifi_direct_r2=true # ro" not in values.splitlines():
        raise AssertionError("Genuine pinned bp4a did not select read-only R2=true")
    (out / "aconfig.values").write_text(values)
    run([sys.executable, str(ROOT / "tools/gen/aconfig_java.py"), str(out / "aconfig.values"),
         str(dirs["generated-flags"]), str(flags.resolve())])
    run([*common, "-cp", classpath, "-d", str(dirs["upstream-classes"]),
         *(str(p) for p in sorted(dirs["upstream-sources"].glob("*.java"))),
         *(str(p) for p in sorted(dirs["generated-flags"].rglob("*.java")))])

    def entries(directory: Path):
        return {str(p.relative_to(directory)): p for p in directory.rglob("*.class")}

    callers = {**entries(dirs["sdk-callers"]), **entries(dirs["extended-classes"])}
    payloads = {"stock": callers, "candidate": {**entries(dirs["candidate-classes"]), **callers},
                "pinned-r4": {**entries(dirs["upstream-classes"]), **callers}, "launcher": entries(dirs["launcher-classes"])}
    dex_manifests = {}
    for name, payload in payloads.items():
        if any(path.startswith(("android/os/", "java/", "dalvik/")) for path in payload):
            raise AssertionError("Framework/core replacement leaked into " + name)
        if name in ("stock", "launcher") and any(not path.startswith("gesher/services/WifiP2pNsdValues") for path in payload):
            raise AssertionError("Stock/launcher payload must contain fixtures only")
        jar(out / (name + "-classes.jar"), payload)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8",
             "--min-api", "36", "--lib", str(args.sdk_jar.resolve()), "--output", str(out / (name + ".jar")),
             str(out / (name + "-classes.jar"))])
        definitions, manifest = members.archive_members(out / (name + ".jar"), "boot", name)
        if any(item.startswith(("android.os.", "java.", "dalvik.")) for item in definitions):
            raise AssertionError("Forbidden DEX provider: " + name)
        dex_manifests[name] = {"manifest": manifest, "defined_classes": sorted(definitions)}

    tracked = [public, extended, launcher, *sources, *(ROOT / path for path in (
        "tools/prepare_wifi_p2p_nsd_oracle.py", "tools/audit_wifi_p2p_nsd_sources.py", "tools/test_wifi_p2p_nsd_values.py",
        "tools/audit_wifi_p2p_sources.py", "tools/prepare_wifi_p2p_oracle.py", "tools/test_wifi_p2p_values.py",
        "tools/java_member_inventory.py", "tools/java_api_inventory.py",
        "tools/gen/aconfig_java.py", "tools/gen/aconfig_values.py", "tools/gen/aconfig_header.py"))]
    for source in tracked:
        target = dirs["source-snapshot"] / "candidate" / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(source.read_bytes())
    manifest = {"source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_files": {str(p.relative_to(ROOT)): sha(p) for p in tracked}, "aosp_wifi_commit": WIFI_COMMIT,
        "aosp_connectivity_commit": CONNECTIVITY_COMMIT, "upstream_sources": provenance,
        "release_profile": {"name": "bp4a", "wifi_direct_r2": {"enabled": True, "read_only": True},
                            "verified_inputs": RELEASE, "all_generated_values_sha256": sha(out / "aconfig.values")},
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar),
        "public_callers": "official SDK36 only", "extended_fixture": "source-level hidden value observers and original CREATORs",
        "native_graph": "all source-owned NSD values plus complete genuine helper; genuine boot Parcel; no fallback",
        "stock_profile": "stock legacy values only; USD optional presence recorded, not qualified",
        "cases": {"stock": 13, "pinned-r4": 20, "candidate": 20, "writer_only": 0},
        "source_preserved_rewrite": "USD Info normalizes null query list to empty; original and rewritten hashes distinct",
        "production_helper": "existing framework provider reused, isolated complete helper is reference payload only",
        "payload_classfiles": {name: {item: sha(path) for item, path in sorted(payload.items())} for name, payload in payloads.items()},
        "dex_providers": dex_manifests,
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(out.glob("*.jar"))}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-p2p-nsd-" + sha(out / "candidate.jar")[:12]
    setup = {"description": "Install frozen ordinary P2P NSD value graphs", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 60} for name in payloads),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30}]}
    (out / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    for profile in ("stock", "pinned-r4", "candidate"):
        request = {"description": "P2P NSD " + profile + " ordinary records with complete original readers", "steps": [
            {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                     "gesher.services.WifiP2pNsdValuesOracle", profile, remote + "/" + profile + ".jar"], "timeout": 120}]}
        (out / (profile + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
    cleanup = {"description": "Remove only owned frozen P2P NSD capsule files", "steps": [
        {"adb": ["shell", "rm", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "rmdir", remote], "timeout": 30}]}
    (out / "cleanup-request.json").write_text(json.dumps(cleanup, indent=2) + "\n")
    print(json.dumps({"output": str(out), "remote": remote, "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
