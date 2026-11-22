#!/usr/bin/env python3
"""Compile immutable normal-value profiles for separate independent review.

Writes requests for the root operator; never runs or queues a device command.
Every record uses a complete original reader and verifies the rewritten bytes.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import zipfile

import java_member_inventory as members
from audit_wifi_channel_blocking_sources import ROOT, SOURCE_DIR, SUPPORT, UPSTREAM, WIFI_COMMIT, audit, sha
from test_wifi_channel_blocking_values import EXTENDED, PACKAGE, PUBLIC, SDK_SHA256, SELECTED, caller_census, check_declarations, graph_sources, write_json

CASE_NAMES = ("channel_sta20", "channel_all20", "channel_direct20", "blocking_min_network", "blocking_min_bssid",
    "blocking_max_network", "blocking_max_bssid", "blocking_snapshot", "channel_width40", "channel_width80",
    "channel_width160", "channel_width80p80", "channel_width320", "blocking_direct_network", "blocking_direct_bssid",
    "blocking_parcel_constructor")


def jar(path: Path, payload: dict[str, Path]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, source in sorted(payload.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, source.read_bytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes", "d8-jar", "upstream-dir", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if sha(args.sdk_jar) != SDK_SHA256: raise AssertionError("Pinned SDK36r02 required")
    source_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    source_tree = subprocess.check_output(["git", "rev-parse", source_head + "^{tree}"], cwd=ROOT, text=True).strip()
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    dirs = {name: out / name for name in ("candidate-classes", "pinned-r4-classes", "sdk-callers", "extended-classes", "launcher-classes", "source-snapshot")}
    for directory in dirs.values(): directory.mkdir()
    commands = []

    def run(argv: list[str]):
        started = time.monotonic()
        result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=180)
        log = f"build-step-{len(commands) + 1:02d}.log"
        (out / log).write_text(result.stdout + result.stderr, encoding="utf-8", newline="\n")
        commands.append({"argv": argv, "exit_code": result.returncode, "seconds": time.monotonic() - started, "output": log})
        write_json(out / "commands.json", commands)
        if result.returncode: raise RuntimeError(f"Capsule step {len(commands)} failed:\n{result.stdout}{result.stderr}")
        return result

    javac, java = (str(args.java_home.resolve() / "bin" / name) for name in ("javac", "java"))
    common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
    cp = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    launcher = ROOT / "tests/wifi-channel-blocking/WifiChannelBlockingValuesOracle.java"
    write_json(out / "source-token-audit.json", audit(args.upstream_dir))
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["sdk-callers"]), str(PUBLIC)])
    provenance = {}
    for profile in ("candidate", "pinned-r4"):
        sources, provenance[profile] = graph_sources(out, profile, args.upstream_dir)
        run([*common, "-cp", cp, "-d", str(dirs[profile + "-classes"]), *map(str, sources)])
        declarations = check_declarations(args.sdk_jar, dirs[profile + "-classes"])
        write_json(out / (profile + "-sdk-members.json"), declarations)
        if profile == "candidate": write_json(out / "sdk-caller-census.json", caller_census(declarations, dirs["sdk-callers"]))
    run([*common, "-cp", os.pathsep.join((str(dirs["candidate-classes"]), str(dirs["sdk-callers"]), cp)),
        "-d", str(dirs["extended-classes"]), str(EXTENDED)])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["launcher-classes"]), str(launcher)])

    def entries(directory: Path) -> dict[str, Path]:
        return {p.relative_to(directory).as_posix(): p for p in directory.rglob("*.class")}

    callers = entries(dirs["sdk-callers"]) | entries(dirs["extended-classes"])
    payloads = {"stock": callers, "candidate": entries(dirs["candidate-classes"]) | callers,
                "pinned-r4": entries(dirs["pinned-r4-classes"]) | callers, "launcher": entries(dirs["launcher-classes"])}
    prefixes = tuple((PACKAGE + name).replace("/", ".") for name in SELECTED)
    marker = "android.net.wifi.WifiAvailableChannel-IA"

    def owned(name: str) -> bool:
        return name.startswith("gesher.services.WifiChannelBlockingValues") or any(
            name == prefix or name.startswith(prefix + "$") for prefix in prefixes)

    dex_manifests = {}
    for profile, payload in payloads.items():
        if any(not owned(path.removesuffix(".class").replace("/", ".")) for path in payload):
            raise AssertionError("Unlisted classfile provider")
        if profile in ("stock", "launcher") and any(not name.startswith("gesher/services/WifiChannelBlockingValues") for name in payload):
            raise AssertionError("Value provider in fixture-only classfile payload")
        jar(out / (profile + "-classes.jar"), payload)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8", "--min-api", "36",
             "--lib", str(args.sdk_jar.resolve()), "--output", str(out / (profile + ".jar")), str(out / (profile + "-classes.jar"))])
        definitions, manifest = members.archive_members(out / (profile + ".jar"), "boot", profile)
        if any(not owned(name) and name != marker for name in definitions):
            raise AssertionError("Unlisted DEX provider: " + profile)
        if (marker in definitions) != (profile in ("candidate", "pinned-r4")):
            raise AssertionError("D8 source constructor access marker census changed")
        if marker in definitions:
            value = definitions[marker]
            if value.flags != 0x1011 or value.superclass != "java.lang.Object" or value.interfaces or value.members:
                raise AssertionError("D8 access marker is not an empty public final synthetic class")
        if profile in ("stock", "launcher") and any(not name.startswith("gesher.services.WifiChannelBlockingValues") for name in definitions):
            raise AssertionError("Implementation leaked into fixture-only DEX")
        dex_manifests[profile] = {"manifest": manifest, "defined_classes": sorted(definitions)}
    tracked = [PUBLIC, EXTENDED, launcher, ROOT / "tests/wifi-channel-blocking/WifiChannelBlockingValuesHost.java",
        SOURCE_DIR / "ScanResult.java", *(SOURCE_DIR / name for name in UPSTREAM),
        *(ROOT / name for name in ("tools/prepare_wifi_channel_blocking_oracle.py", "tools/audit_wifi_channel_blocking_sources.py",
            "tools/test_wifi_channel_blocking_values.py", "tools/java_member_inventory.py", "tools/java_api_inventory.py"))]
    for source in tracked:
        committed = subprocess.check_output(["git", "show", source_head + ":" + source.relative_to(ROOT).as_posix()], cwd=ROOT)
        if source.read_bytes() != committed:
            raise AssertionError("Capsule input differs from the committed source head: " + str(source))
        target = dirs["source-snapshot"] / "candidate" / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(committed)
    for name, expected in {**UPSTREAM, **SUPPORT}.items():
        raw = args.upstream_dir / name
        if sha(raw) != expected: raise AssertionError("Pinned source changed during build")
        target = dirs["source-snapshot"] / "upstream" / name
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(raw.read_bytes())
    manifest = {
        "source_head": source_head,
        "source_tree": source_tree,
        "source_files": {p.relative_to(ROOT).as_posix(): sha(p) for p in tracked},
        "wifi_commit": WIFI_COMMIT, "source_provenance": provenance,
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar), "app_compile_jar_sha256": sha(args.app_classes),
        "compiler_sha256": {name: sha(args.java_home / "bin" / name) for name in ("java", "javac")},
        "compiler_modules_sha256": sha(args.java_home / "lib/modules"),
        "framework_compile_class_sha256": {name: sha(args.framework_classes / name) for name in (
            "android/os/Parcel.class", "android/os/Parcelable.class", "android/os/Build.class", "android/os/Build$VERSION.class",
            "android/annotation/IntDef.class", "android/annotation/IntRange.class",
            "android/annotation/NonNull.class", "android/annotation/Nullable.class")},
        "public_callers": "Official SDK36 only: 21 direct references and six inlined constants checked as runtime fields",
        "extended_callers": "Separate hidden constructors, SOURCE annotations and filter values",
        "owned_graph": "Two complete values, Builder, original creators and two annotation declarations",
        "genuine_boot_leaves": ["Parcel", "Parcelable", "Build", "Object", "Objects"],
        "scan_result_runtime_dependency": False, "sdk_definitions_packaged": False,
        "cases": {"candidate": list(CASE_NAMES), "pinned-r4": list(CASE_NAMES), "stock_if_complete": list(CASE_NAMES), "writer_only": []},
        "stock_profile": "All types and used hidden/flagged members observed before typed fixture loading; absence cannot qualify",
        "native_observations": "Pending root execution after separate independent preflight; historical fixture counts do not qualify these rebuilt fixtures",
        "payload_classfiles": {name: {item: sha(path) for item, path in sorted(payload.items())} for name, payload in payloads.items()},
        "dex_providers": dex_manifests,
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(out.glob("*.jar"))},
    }
    if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip() != source_head:
        raise AssertionError("Source head changed during capsule compilation")
    write_json(out / "manifest.json", manifest)
    remote = "/data/local/tmp/gesher-wifi-channel-blocking-" + sha(out / "candidate.jar")[:12]
    write_json(out / "setup-request.json", {"description": "Install frozen ordinary channel/blocking value profiles", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 60} for name in payloads),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30}]})
    for profile in ("stock", "pinned-r4", "candidate"):
        write_json(out / (profile + "-request.json"), {"description": profile + " ordinary channel/blocking records through full original readers", "steps": [
            {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                "gesher.services.WifiChannelBlockingValuesOracle", profile, remote + "/" + profile + ".jar"], "timeout": 120}]})
    write_json(out / "cleanup-request.json", {"description": "Remove only owned frozen channel/blocking capsule files", "steps": [
        {"adb": ["shell", "rm", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "rmdir", remote], "timeout": 30}]})
    print(json.dumps({"output": str(out), "remote": remote, "artifacts": manifest["artifacts"]}, indent=2))


if __name__ == "__main__":
    main()
