#!/usr/bin/env python3
"""SDK36 callers over freshly compiled complete channel/blocking value sources.

Host validation supplies no Parcel implementation and never executes Parcel.
Native reader qualification is a separate Android/Windows execution gate.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import zipfile

import java_member_inventory as members
from audit_wifi_channel_blocking_sources import ROOT, SOURCE_DIR, UPSTREAM, adapt_value, audit, sha

SDK_SHA256 = "d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a"
PACKAGE = "android/net/wifi/"
SELECTED = ("WifiAvailableChannel", "BlockingOption", "BlockingOption$Builder")
EXPECTED_COUNTS = (16, 8, 3)
CONSTANTS = {"OP_MODE_STA", "OP_MODE_SAP", "OP_MODE_WIFI_DIRECT_CLI", "OP_MODE_WIFI_DIRECT_GO", "OP_MODE_WIFI_AWARE", "OP_MODE_TDLS"}
PUBLIC = ROOT / "tests/dex-services/gesher/services/WifiChannelBlockingValuesContractTest.java"
EXTENDED = ROOT / "tests/dex-services/gesher/services/WifiChannelBlockingValuesExtendedTest.java"


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
                raise AssertionError("SDK denominator changed: " + name)
            rows[expected.name] = {"declarations": declarations, "compiled_sha256": sha(classes / path)}
    return {"scope": "SDK36 declared public/protected presence, not behavior coverage", "sdk_sha256": SDK_SHA256,
            "classes": rows, "total_declarations": sum(EXPECTED_COUNTS), "hidden_contracts_separate": True}


def member_references(data: bytes) -> set[tuple[str, str, str, str]]:
    """Constant-pool member references from the valid, freshly compiled SDK caller."""
    members.class_members(data)
    pool, at, index = {}, 10, 1
    count = struct.unpack_from(">H", data, 8)[0]
    while index < count:
        tag = data[at]; at += 1
        if tag == 1:
            length = struct.unpack_from(">H", data, at)[0]; at += 2
            pool[index] = (tag, members.modified_utf8(data[at:at + length])); at += length
        elif tag in (7, 8, 16, 19, 20):
            pool[index] = (tag, struct.unpack_from(">H", data, at)[0]); at += 2
        elif tag in (9, 10, 11, 12, 17, 18):
            pool[index] = (tag, *struct.unpack_from(">HH", data, at)); at += 4
        elif tag in (3, 4): at += 4
        elif tag in (5, 6): at += 8; index += 1
        elif tag == 15: at += 3
        else: raise AssertionError("Unexpected compiler pool tag")
        index += 1
    refs = set()
    for value in pool.values():
        if value[0] in (9, 10, 11):
            owner = pool[pool[value[1]][1]][1].replace("/", ".")
            _, name, descriptor = pool[value[2]]
            refs.add((owner, "field" if value[0] == 9 else "method", pool[name][1], pool[descriptor][1]))
    return refs


def caller_census(declarations: dict, callers: Path) -> dict:
    paths = sorted(callers.rglob("*.class"))
    refs = set().union(*(member_references(path.read_bytes()) for path in paths))
    rows = []
    for owner, value in declarations["classes"].items():
        for declaration in value["declarations"]:
            key = (owner, declaration["kind"], declaration["name"], declaration["descriptor"])
            constant = owner == "android.net.wifi.WifiAvailableChannel" and declaration["kind"] == "field" and declaration["name"] in CONSTANTS
            if key not in refs and not constant:
                raise AssertionError("Missing direct SDK-only caller reference: " + str(key))
            rows.append({"owner": owner, **declaration, "evidence": "SDK-inlined constant, separately runtime-reflected" if constant else "direct SDK-only caller reference"})
    direct = sum(row["evidence"].startswith("direct") for row in rows)
    if len(rows) != 27 or direct != 21:
        raise AssertionError("SDK caller denominator changed")
    return {"sdk_only": True, "declarations": rows, "total": len(rows), "direct_references": direct,
            "inlined_constants_with_runtime_field_checks": 6,
            "classfiles_sha256": {p.relative_to(callers).as_posix(): sha(p) for p in paths}}


def graph_sources(work: Path, profile: str, upstream: Path) -> tuple[list[Path], dict]:
    sources, provenance = [], {}
    generated = work / (profile + "-sources"); generated.mkdir()
    for name, expected in UPSTREAM.items():
        raw = upstream / name
        if sha(raw) != expected: raise AssertionError("Pinned source changed: " + name)
        if profile == "candidate":
            target = SOURCE_DIR / name
            changes = {"provider": "candidate production source"}
        else:
            target = generated / name
            adapted, changes = adapt_value(raw.read_text(encoding="utf-8"), name)
            target.write_text(adapted, encoding="utf-8", newline="\n")
        sources.append(target)
        provenance[name] = {"raw_sha256": expected, "compiled_source_sha256": sha(target), **changes, "complete_source": True}
    return sources, provenance


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes", "upstream-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--profile", choices=("candidate", "pinned-r4"), default="candidate")
    parser.add_argument("--source-ref", help="Exact absence baseline for both new production sources")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.source_ref and args.profile != "candidate": parser.error("Separate baseline and pinned-source profiles")
    if sha(args.sdk_jar) != SDK_SHA256: parser.error("Pinned official SDK36r02 required")
    cp = os.pathsep.join(str(p.resolve()) for p in (args.app_classes, args.framework_classes, args.core_classes))
    javac, java = (str(args.java_home.resolve() / "bin" / name) for name in ("javac", "java"))
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve(); work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-channel-blocking-")))
        classes, callers, extended, host = (work / name for name in ("classes", "sdk-callers", "extended", "host"))
        for directory in (classes, callers, extended, host): directory.mkdir()
        commands = []

        def run(argv: list[str], *, expected: int = 0):
            started = time.monotonic()
            result = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=120)
            log = f"step-{len(commands) + 1:02d}.log"
            (work / log).write_text(result.stdout + result.stderr, encoding="utf-8", newline="\n")
            commands.append({"argv": argv, "exit_code": result.returncode, "expected_exit": expected,
                             "seconds": time.monotonic() - started, "output": log})
            write_json(work / "commands.json", commands)
            if result.returncode != expected:
                raise RuntimeError(f"Step {len(commands)} failed:\n{result.stdout}{result.stderr}")
            return result

        common = [javac, "-J-XX:-UsePerfData", "-J-Xmx768m", "-encoding", "UTF-8", "-sourcepath", ""]
        run([*common, "-cp", str(args.sdk_jar.resolve()), "-d", str(callers), str(PUBLIC)])
        provenance = {}
        if args.source_ref:
            revision = subprocess.check_output(["git", "rev-parse", args.source_ref + "^{commit}"], cwd=ROOT, text=True).strip()
            for name in UPSTREAM:
                path = (SOURCE_DIR / name).relative_to(ROOT).as_posix()
                if subprocess.check_output(["git", "ls-tree", "--name-only", revision, "--", path], cwd=ROOT):
                    raise AssertionError("Selected provider exists at requested absence baseline: " + path)
        else:
            write_json(work / "source-audit.json", audit(args.upstream_dir))
            sources, provenance = graph_sources(work, args.profile, args.upstream_dir)
            run([*common, "-cp", cp, "-d", str(classes), *map(str, sources)])
            declarations = check_declarations(args.sdk_jar, classes)
            write_json(work / "sdk-members.json", declarations)
            write_json(work / "sdk-caller-census.json", caller_census(declarations, callers))
            run([*common, "-cp", os.pathsep.join((str(classes), str(callers), cp)), "-d", str(extended), str(EXTENDED)])
        run([*common, "-cp", str(callers), "-d", str(host), str(ROOT / "tests/wifi-channel-blocking/WifiChannelBlockingValuesHost.java")])
        result = run([java, "-XX:-UsePerfData", "-cp", os.pathsep.join((str(classes), str(host), str(callers), str(extended), cp)),
            "gesher.services.WifiChannelBlockingValuesHost", str(classes), str(callers), str(args.framework_classes.resolve()),
            "baseline" if args.source_ref else args.profile], expected=1 if args.source_ref else 0)
        if args.source_ref:
            output = result.stdout + result.stderr
            missing = "android.net.wifi.WifiAvailableChannel"
            if "ClassNotFoundException: " + missing not in output or "Stub!" in output or "PASS" in output:
                raise AssertionError("Absence baseline did not fail at the missing provider")
            write_json(work / "negative-control.json", {"source_revision": revision, "missing": missing,
                "exit_code": result.returncode, "compilation_stub_fallback": False})
            print("Exact baseline lacks the genuine WifiAvailableChannel provider; SDK fallback rejected")
        else:
            print(result.stdout, end="")
            write_json(work / "providers.json", {"profile": args.profile, "source_provenance": provenance,
                "compiled_classes": {p.relative_to(classes).as_posix(): sha(p) for p in sorted(classes.rglob("*.class"))},
                "public_fixture_sdk_only": True, "native_parcel_execution": False,
                "framework_parcel_sha256": sha(args.framework_classes / "android/os/Parcel.class"),
                "existing_app_compile_jar_sha256": sha(args.app_classes)})


if __name__ == "__main__":
    main()
