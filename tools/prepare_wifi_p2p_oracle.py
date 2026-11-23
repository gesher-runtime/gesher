#!/usr/bin/env python3
"""Build immutable API36 P2P reference capsules and root-owned queue manifests.

No device commands execute here. All Android Parcel providers remain genuine boot
classes. Source-owned DeviceList profiles write ordinary bytes but never decode
through an incoherent null-loader graph. Windows requires the complete reader.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile

from test_wifi_p2p_values import FAMILY, P2P, ROOT, SDK_SHA256, check_declarations, sha
import java_member_inventory as members

COMMIT = "e3f09203409da7a91d2c5cf9f0109da6d5243319"
UPSTREAM = {
    "WifiP2pConfig.java": "690eacbbba8d04a30b2134ddef17a0b1ebbb6d76c6ce62ff4edcd5aa5574f325",
    "WifiP2pPairingBootstrappingConfig.java": "df3a85270e801260c4373204bb8042b6a136f81f07b5f2bc92f47c0a8c045a11",
    "WifiP2pDevice.java": "c28e9a3f7ef947a4c905e13c2937ec2ef68f253885e8c2b9a6b7a621e7876b42",
    "WifiP2pWfdInfo.java": "22266040a6194335d00134a50efbe824b5f8fac245d290b9b50f11749f39b057",
    "WifiP2pDeviceList.java": "89b006d2c20f1f185685cd5e348d88eeaa602b41ef62b522b02e095337fd6fc8",
    "WifiP2pGroup.java": "3e59859ba6da1d26679b68777fd7db08f94de3552be3af4b2a47121275033255",
    "WifiP2pInfo.java": "87cf578d032cb46c3c92b939bc3c9d49991070388021d6e0501d2bd4e5d1d7c2",
    "WifiP2pDirInfo.java": "2efeef247f555f6cbc0b20dd72a2dd0b9b6ae8da9868ca509b9ee65dee8090a4",
}
SUPPORT = {
    "framework/java/android/net/wifi/util/Environment.java": "883021cad62185fd0ff650ef7b309888f4661268179b0f48b61caac3dd6a106d",
    "flags/wifi_flags.aconfig": "4b1c143125721e07d24c664496958f16134d5d2f72b2376e27fdd573579666f0",
}
RELEASE = {
    "release_configs/bp4a.textproto": "3f0a1de927826a70b2165ea6b2b378b87a78995d30645b44e741d82b5792871a",
    "release_configs/bp3a.textproto": "31b921df4dc487f4aa4dcb5ff3a019db1899817a1d134506541066b35cd2810b",
    "release_configs/bp2a.textproto": "ec6eff13fe0f08bed831f59a50bc33289efdcbb5136a618dfae18cee0cda9e5f",
    "aconfig/bp2a/com.android.wifi.flags/wifi_direct_r2_flag_values.textproto": "22dfdbf299d52f56193a4036f2ad4a310b99107e448c298b6e7e21517c77420f",
}


def jar(path: Path, entries: dict[str, Path]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, source in sorted(entries.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, source.read_bytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes",
                 "d8-jar", "p2p-source-dir", "wifi-support-dir", "release-dir", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if sha(args.sdk_jar) != SDK_SHA256:
        raise AssertionError("Pinned SDK36 required")
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
    public = ROOT / "tests/dex-services/gesher/services/WifiP2pValuesContractTest.java"
    extended = ROOT / "tests/dex-services/gesher/services/WifiP2pValuesExtendedTest.java"
    launcher = ROOT / "tests/wifi-p2p/WifiP2pValuesOracle.java"
    sources = [ROOT / "app/java/android/net" / (name + ".java") for name in FAMILY]
    run([sys.executable, str(ROOT / "tools/audit_wifi_p2p_sources.py"),
         "--upstream-dir", str(args.p2p_source_dir.resolve()), "--output", str(out / "source-token-audit.json")])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["sdk-callers"]), str(public)])
    run([*common, "-cp", classpath, "-d", str(dirs["candidate-classes"]), *map(str, sources)])
    inventory = check_declarations(args.sdk_jar, dirs["candidate-classes"])
    (out / "sdk-members.json").write_text(json.dumps(inventory, indent=2) + "\n")
    run([*common, "-cp", os.pathsep.join((str(dirs["candidate-classes"]), str(dirs["sdk-callers"]), classpath)),
         "-d", str(dirs["extended-classes"]), str(extended)])
    run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(dirs["launcher-classes"]), str(launcher)])

    originals = {"p2p/" + name: args.p2p_source_dir / name for name in UPSTREAM}
    expected = {"p2p/" + name: value for name, value in UPSTREAM.items()}
    originals["util/Environment.java"] = args.wifi_support_dir / "framework/java/android/net/wifi/util/Environment.java"
    expected["util/Environment.java"] = SUPPORT["framework/java/android/net/wifi/util/Environment.java"]
    provenance = {}
    for name, path in originals.items():
        if sha(path) != expected[name]: raise AssertionError("Upstream hash mismatch: " + str(path))
        raw = path.read_text()
        text = re.sub(r"(?m)^import (android\.annotation\.FlaggedApi|androidx\.annotation\.RequiresApi);\n", "", raw)
        text, count = re.subn(r"(?m)^[ \t]*@(FlaggedApi|RequiresApi)\([^\n]*\)[ \t]*\n", "", text)
        text = text.replace("import com.android.modules.utils.build.SdkLevel;", "import android.internal.modules.utils.build.SdkLevel;")
        target = dirs["upstream-sources"] / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_text(text)
        raw_target = dirs["source-snapshot"] / "upstream" / name; raw_target.parent.mkdir(parents=True, exist_ok=True); raw_target.write_bytes(path.read_bytes())
        provenance[name] = {"url": "https://android.googlesource.com/platform/packages/modules/Wifi/+/" + COMMIT
                + "/framework/java/android/net/wifi/" + name,
                "raw_sha256": sha(path), "compiled_source_sha256": sha(target), "removed_build_annotations": count,
                "adaptations": ["Unavailable build annotations removed", "Genuine framework jarjar SdkLevel namespace"],
                "hidden_dir_field_and_update_retained": name in ("p2p/WifiP2pDevice.java", "p2p/WifiP2pDeviceList.java")}
    for name, expected_sha in RELEASE.items():
        if sha(args.release_dir / name) != expected_sha: raise AssertionError("Release profile input changed: " + name)
    flags = args.wifi_support_dir / "flags/wifi_flags.aconfig"
    if sha(flags) != SUPPORT["flags/wifi_flags.aconfig"]: raise AssertionError("Flag declaration changed")
    generated_values = run([sys.executable, str(ROOT / "tools/gen/aconfig_values.py"), str(args.release_dir), "bp4a"]).stdout
    if "com.android.wifi.flags.wifi_direct_r2=true # ro" not in generated_values.splitlines():
        raise AssertionError("Pinned bp4a read-only R2 value was not selected")
    (out / "aconfig.values").write_text(generated_values)
    run([sys.executable, str(ROOT / "tools/gen/aconfig_java.py"), str(out / "aconfig.values"), str(dirs["generated-flags"]), str(flags)])
    run([*common, "-cp", os.pathsep.join((str(dirs["candidate-classes"]), classpath)), "-d", str(dirs["upstream-classes"]),
         *(str(p) for p in sorted(dirs["upstream-sources"].rglob("*.java"))),
         *(str(p) for p in sorted(dirs["generated-flags"].rglob("*.java")))])

    def entries(directory):
        return {str(p.relative_to(directory)): p for p in directory.rglob("*.class")}
    candidate = entries(dirs["candidate-classes"])
    dependencies = {name: path for name, path in candidate.items() if not name.startswith("android/net/wifi/p2p/")}
    public_entries = entries(dirs["sdk-callers"])
    extended_entries = entries(dirs["extended-classes"])
    payloads = {"stock": public_entries, "candidate": {**candidate, **public_entries, **extended_entries},
                "pinned-r4": {**dependencies, **entries(dirs["upstream-classes"]), **public_entries, **extended_entries},
                "launcher": entries(dirs["launcher-classes"])}
    dex_manifests = {}
    for name, payload in payloads.items():
        if any(path.startswith(("android/os/", "java/", "dalvik/")) for path in payload):
            raise AssertionError("Framework/core replacement leaked into " + name)
        jar(out / (name + "-classes.jar"), payload)
        run([java, "-XX:-UsePerfData", "-cp", str(args.d8_jar.resolve()), "com.android.tools.r8.D8",
             "--min-api", "36", "--lib", str(args.sdk_jar.resolve()), "--output", str(out / (name + ".jar")),
             str(out / (name + "-classes.jar"))])
        definitions, manifest = members.archive_members(out / (name + ".jar"), "boot", name)
        if any(item.startswith(("android.os.", "java.", "dalvik.")) for item in definitions):
            raise AssertionError("Forbidden DEX provider in " + name)
        dex_manifests[name] = {"manifest": manifest, "defined_classes": sorted(definitions)}

    tracked_sources = [public, extended, launcher, *sources, ROOT / "tools/prepare_wifi_p2p_oracle.py",
                       ROOT / "tools/audit_wifi_p2p_sources.py",
                       ROOT / "tools/test_wifi_p2p_values.py", ROOT / "tools/gen/aconfig_java.py",
                       ROOT / "tools/gen/aconfig_values.py", ROOT / "tools/gen/aconfig_header.py"]
    for source in tracked_sources:
        target = dirs["source-snapshot"] / "candidate" / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(source.read_bytes())
    manifest = {"source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_files": {str(p.relative_to(ROOT)): sha(p) for p in tracked_sources},
        "aosp_wifi_commit": COMMIT, "upstream_sources": provenance,
        "release_profile": {"name": "bp4a", "wifi_direct_r2": {"enabled": True, "read_only": True},
                            "verified_inputs": RELEASE, "all_generated_values_sha256": sha(out / "aconfig.values")},
        "sdk_sha256": sha(args.sdk_jar), "d8_sha256": sha(args.d8_jar),
        "scope": "Normal native Parcel; stock profile measured separately; source-owned DeviceList writer only",
        "public_callers": "SDK36 only", "extended_fixture": "Source-level value setup, no Android implementation",
        "source_owned_prerequisites": "Reviewed PR144 dependency sources, unchanged in candidate and pinned profiles",
        "stock_profile": "System Wi-Fi module; normal legacy/public subset, optional R2 presence recorded",
        "device_list_reader": {"stock": True, "candidate": False, "pinned-r4": False, "required_windows": True},
        "payload_classfiles": {name: {item: sha(path) for item, path in sorted(payload.items())} for name, payload in payloads.items()},
        "dex_providers": dex_manifests,
        "artifacts": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in sorted(out.glob("*.jar"))}}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    remote = "/data/local/tmp/gesher-p2p-values-" + sha(out / "candidate.jar")[:12]
    setup = {"description": "Install frozen normal P2P value/profile capsules", "steps": [
        {"adb": ["shell", "mkdir", "-p", remote], "timeout": 30},
        *({"adb": ["push", str(out / (name + ".jar")), remote + "/" + name + ".jar"], "timeout": 60} for name in payloads),
        {"adb": ["shell", "chmod", "0444", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30},
        {"adb": ["shell", "sha256sum", *(remote + "/" + name + ".jar" for name in payloads)], "timeout": 30}]}
    (out / "setup-request.json").write_text(json.dumps(setup, indent=2) + "\n")
    for profile in ("stock", "pinned-r4", "candidate"):
        request = {"description": "P2P " + profile + " normal values with verified native providers", "steps": [
            {"adb": ["shell", "env", "CLASSPATH=" + remote + "/launcher.jar", "app_process", "/system/bin",
                     "gesher.services.WifiP2pValuesOracle", profile, remote + "/" + profile + ".jar"], "timeout": 120}]}
        (out / (profile + "-request.json")).write_text(json.dumps(request, indent=2) + "\n")
    print(json.dumps({"output": str(out), "remote": remote,
                      "artifacts": {name + ".jar": manifest["artifacts"][name + ".jar"] for name in payloads}}, indent=2))


if __name__ == "__main__":
    main()
