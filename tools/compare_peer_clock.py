#!/usr/bin/env python3
"""Run one native ELF on Android API 36 and Windows Gesher, then compare observations."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile


SOURCE = Path(__file__).resolve().parents[1] / "tests/native/peer_clock_differential.c"
TARGETS = {
    "x86_64": ("x86_64-linux-android", 62),
    "arm64-v8a": ("aarch64-linux-android", 183),
}
FIELDS = {
    "elf_machine",
    "positive_resolution",
    "same_live_id",
    "peer_delta_ns",
    "caller_delta_ns",
    "retired_gettime_errno",
    "retired_getres_errno",
    "capacity_acquired",
    "capacity_distinct",
    "capacity_first_errno",
    "complete",
}
MARKER = "PEER_CLOCK_V1 "


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(args, *, timeout=30, cwd=None):
    return subprocess.run(args, timeout=timeout, cwd=cwd, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def successful(process, step):
    require(process.returncode == 0,
            f"{step} exited {process.returncode}: {process.stderr.strip()}")
    return process.stdout.strip()


def capture(process, evidence, key):
    evidence[key] = {
        "exit_code": process.returncode,
        "stdout": process.stdout,
        "stderr": process.stderr,
    }
    return successful(process, f"{key} fixture")


def observations(stdout, machine):
    values = {}
    for line in stdout.splitlines():
        if not line.startswith(MARKER):
            continue
        row = re.fullmatch(r"([a-z0-9_]+)=([0-9]{1,19})", line[len(MARKER):])
        require(row is not None, f"malformed fixture row: {line}")
        key, value = row.groups()
        require(key in FIELDS and key not in values, f"unexpected or duplicate fixture row: {key}")
        values[key] = int(value)
    require(values.keys() == FIELDS, f"missing fixture rows: {sorted(FIELDS - values.keys())}")
    require(next(reversed(values)) == "complete" and values["complete"] == 1,
            "missing terminal fixture marker")
    require(values["elf_machine"] == machine, "fixture ABI mismatch")
    require(values["positive_resolution"] <= 1 and values["same_live_id"] <= 1024,
            "invalid live clock observations")
    acquired = values["capacity_acquired"]
    require(values["capacity_distinct"] <= acquired <= 257, "invalid capacity observations")
    require((values["capacity_first_errno"] == 0) == (acquired == 257),
            "inconsistent capacity errno")
    peer = values["peer_delta_ns"]
    caller = values["caller_delta_ns"]
    return {
        "positive_resolution": values["positive_resolution"],
        "same_live_id": values["same_live_id"],
        "peer_advances_independently": peer >= 50_000_000 and peer > caller,
        "retired_gettime_errno": values["retired_gettime_errno"],
        "retired_getres_errno": values["retired_getres_errno"],
        "capacity_acquired": acquired,
        "capacity_distinct": values["capacity_distinct"],
        "capacity_first_errno": values["capacity_first_errno"],
    }


def compare(android, windows):
    require(android["positive_resolution"] == 1 and android["same_live_id"] == 1024 and
            android["peer_advances_independently"] and android["retired_gettime_errno"] != 0 and
            android["capacity_distinct"] == 257,
            "Android did not establish a usable peer clock reference")
    return {key: {"android": value, "windows": windows[key]}
            for key, value in android.items() if windows[key] != value}


def android_run(adb, fixture, digest, expected_fingerprint, abi, report):
    def shell(*args, timeout=30):
        return run([*adb, "shell", "-T", *args], timeout=timeout)

    properties = {}
    for name in ("ro.build.fingerprint", "ro.build.version.sdk", "ro.product.cpu.abilist"):
        properties[name] = successful(shell("getprop", name), name)
    report["android_profile"] = properties
    require(expected_fingerprint and properties["ro.build.fingerprint"] == expected_fingerprint,
            "Android fingerprint mismatch")
    require(properties["ro.build.version.sdk"] == "36", "Android reference must be API 36")
    require(abi in properties["ro.product.cpu.abilist"].split(","), "Android reference ABI unavailable")
    directory = successful(shell("mktemp", "-d", "/data/local/tmp/gesher-peer-clock.XXXXXXXX"),
                           "create Android scratch directory")
    require(re.fullmatch(r"/data/local/tmp/gesher-peer-clock\.[A-Za-z0-9]+", directory),
            "unexpected Android scratch path")
    remote = f"{directory}/peer-clock"
    try:
        successful(run([*adb, "push", str(fixture), remote]), "push Android fixture")
        successful(shell("chmod", "500", remote), "make Android fixture executable")
        receipt = successful(shell("sha256sum", remote), "hash Android fixture")
        require(receipt.split() == [digest, remote], "Android fixture hash mismatch")
        return capture(shell("timeout", "90", remote, timeout=105), report, "android")
    finally:
        successful(shell("rm", "-f", remote), "remove Android scratch fixture")
        successful(shell("rmdir", directory), "remove Android scratch directory")


def collect(args, report):
    require(sys.platform == "win32", "the differential run requires a Windows host")
    launcher = args.gesher_exec.resolve(strict=True)
    require(launcher.name.lower() == "gesher-exec.exe", "provide the Gesher ELF launcher")
    report["windows_profile"] = {"version": platform.version(), "host_machine": platform.machine()}
    components = [launcher]
    if args.abi == "arm64-v8a":
        components.append(launcher.with_name("gesher_arm64.dll"))
    hashes = {part.name: sha256(part) for part in components}
    report["windows_components_sha256"] = hashes

    triple, machine = TARGETS[args.abi]
    clang = args.ndk.resolve() / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    ndk_version = (args.ndk / "source.properties").read_text(encoding="utf-8").strip()
    report["ndk_source_properties"] = ndk_version
    report["fixture_source_sha256"] = sha256(SOURCE)
    report["compiler_version"] = successful(run([str(clang), "--version"]), "identify NDK compiler")
    with tempfile.TemporaryDirectory(prefix="gesher-peer-clock-") as work:
        fixture = Path(work) / "peer-clock"
        successful(run([str(clang), f"--target={triple}35", "-std=c11", "-O2", "-Wall",
                        "-Wextra", "-Werror", "-pthread", str(SOURCE), "-o", str(fixture)],
                       timeout=90), "build Android fixture")
        with fixture.open("rb") as stream:
            elf = stream.read(20)
        require(elf[:6] == b"\x7fELF\x02\x01" and int.from_bytes(elf[18:20], "little") == machine,
                "unexpected fixture ELF class or machine")
        digest = sha256(fixture)
        report["fixture_elf_sha256"] = digest
        adb = [args.adb, "-s", args.serial]
        reference = android_run(adb, fixture, digest, args.fingerprint, args.abi, report)
        require(sha256(fixture) == digest, "fixture changed before Windows launch")
        windows = capture(run([str(launcher), "--elf", str(fixture), "peer-clock"], timeout=180,
                              cwd=launcher.parent), report, "windows")
        require(sha256(fixture) == digest, "fixture changed during Windows launch")
        require(hashes == {part.name: sha256(part) for part in components},
                "Windows component changed during the differential run")
    report["android_observations"] = observations(reference, machine)
    report["windows_observations"] = observations(windows, machine)
    return compare(report["android_observations"], report["windows_observations"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--fingerprint", required=True, help="expected Android API 36 build fingerprint")
    parser.add_argument("--abi", required=True, choices=TARGETS)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--gesher-exec", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True, help="new JSON result path")
    args = parser.parse_args()
    report = {"schema": 1, "guest_abi": args.abi, "fixture_min_sdk": 35}
    with args.report.open("x", encoding="utf-8") as stream:
        try:
            report["differences"] = collect(args, report)
            report["status"] = "different" if report["differences"] else "matched"
            code = 1 if report["differences"] else 0
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            report.update(status="inconclusive", error=str(error))
            code = 2
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"Peer clock differential: {report['status']} ({args.report})")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
