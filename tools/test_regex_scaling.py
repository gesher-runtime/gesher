#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Exercise actual regex ABI, bounded NFA work, and optional pinned Android parity.

Normal/timed binaries compile the production wrapper and header unchanged.
A separate counter-only build inserts counters in ExecuteNfa without changing
branches. --compare-ref compares the same seeded cases against historical code.
The optional Android oracle downloads hash-pinned, unmodified BSD sources; only
host annotations/includes and exported symbol names are adapted.
"""
from __future__ import annotations

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BASELINE = "bf7c6e2f1adac284bd00971734f7b50c63d72a94"
ANDROID = "f22516cbc67e81c13cf943ce02f01b8929a98726"
ANDROID_HASHES = {
    "utils.h": "7729fcdd7c269cd66ccf707802df9c8e32db21e8a4b17e528432edbb36468312",
    "regex2.h": "ba977b0ef5a3365be74423bfe49fca3223e3b33745aaeba7c09cf10dd292caeb",
    "engine.c": "c541eb37440b60fa7ffd74ca7bfd7deb5b482d945a9dd31893296e6da1e41ae1",
    "regexec.c": "8160d06bb401d8bcfa510954d06c5cd9c13bc765dafd13590c66e401922473e6",
    "cname.h": "51a618a5e68bb1c5fb0c736ccf5733423f9b04ed25026150119a881efd1fd58a",
    "regfree.c": "251771e0b34a2ee0260ee1314b0bd5f5a4595d35c3c1e2b254668a548e0ba697",
    "regerror.c": "912d0f77e99952690a28974569e976b9bf6f824875c3e6d4bc584f087804e989",
    "regex.h": "995a0436934bf14eb7c989d1ed8a0b6a736468e00b68246ef99d2c8c32322f57",
    "regcomp.c": "8308c820bff17d4b1e33e2a28b6c1303aede890e6b82b63f8a584e9f10f3dc82",
    "namespace.h": "a4df4815ab597fe63d127f21e6b9216d97b6051ebf686b51d63a00343d5f7b55",
    "netbsd-compat.h": "0edbdb9ad49bd6228faa0cee9060b3991cc9c85708437fe868a330af21baabb4",
}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_source(name: str, revision: str | None) -> bytes:
    path = "compat/bionic/" + name
    return (subprocess.check_output(["git", "show", revision + ":" + path], cwd=ROOT)
            if revision else (ROOT / path).read_bytes())


def counted(header: str) -> str:
    begin = header.index("  int ExecuteNfa(")
    end = header.index("  // Filter the complete expression", begin)
    body = header[begin:end]
    inserts = {
        "int32_t pc = stack.back();": "++regex_work::popped;\n        int32_t pc = stack.back();",
        "const Insn& insn = prog_.insns[pc];":
            "++regex_work::expanded;\n        const Insn& insn = prog_.insns[pc];",
        "unsigned char ch = static_cast<unsigned char>(begin_[pos]);":
            "++regex_work::input;\n      unsigned char ch = static_cast<unsigned char>(begin_[pos]);",
    }
    for old, new in inserts.items():
        if body.count(old) != 1:
            raise ValueError("missing or ambiguous NFA counter anchor: " + old)
        body = body.replace(old, new, 1)
    header = header[:begin] + body + header[end:]
    marker = "namespace gesher::bionic::posix_regex {"
    if header.count(marker) != 1:
        raise ValueError("missing or ambiguous regex namespace")
    return header.replace(marker, "namespace regex_work { extern uint64_t popped, expanded, input; }\n"
                          + marker, 1)


def android_objects(folder: Path, cache: Path, cc: str, sanitize: bool) -> list[str]:
    cache.mkdir(parents=True, exist_ok=True)
    def fetch(name: str) -> None:
        path = cache / name
        if not path.exists():
            if name == "regex.h":
                source = "libc/include/" + name
            elif name in ("namespace.h", "netbsd-compat.h"):
                source = "libc/upstream-netbsd/android/include/" + name
            else:
                source = "libc/upstream-netbsd/lib/libc/regex/" + name
            url = f"https://android.googlesource.com/platform/bionic/+/{ANDROID}/{source}?format=TEXT"
            data = base64.b64decode(urllib.request.urlopen(url, timeout=45).read())
            if sha(data) != ANDROID_HASHES[name]:
                raise ValueError("Android download hash mismatch: " + name)
            path.write_bytes(data)
        if sha(path.read_bytes()) != ANDROID_HASHES[name]:
            raise ValueError("Android source hash mismatch: " + name)
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(fetch, ANDROID_HASHES))
    prelude = folder / "android-host-annotations.h"
    prelude.write_text(
        '#define _GNU_SOURCE 1\n#define _DEFAULT_SOURCE 1\n#define __RCSID(x)\n'
        '#define __unused __attribute__((unused))\n#define __LIBC_HIDDEN__\n'
        '#define __BIONIC_COMPLICATED_NULLNESS\n#define _Nonnull\n#define _Nullable\n'
        '#include "netbsd-compat.h"\n#include <stdint.h>\n')
    objects = []
    for name in ("regcomp", "regexec", "regfree", "regerror"):
        target = folder / ("android-" + name + ".o")
        command = shlex.split(cc) + ["-std=gnu11", "-O2", "-g", "-DPOSIX_MISTAKE",
                                    "-include", str(prelude), "-I", str(cache)]
        command += ["-D" + symbol + "=android_" + symbol
                    for symbol in ("regcomp", "regexec", "regfree", "regerror")]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command + ["-c", str(cache / (name + ".c")), "-o", str(target)],
                       check=True, timeout=90)
        objects.append(str(target))
    return objects


def build(folder: Path, label: str, revision: str | None, args, objects: list[str],
          instrument: bool = False, bounds: bool = False) -> tuple[Path, dict]:
    source_dir = folder / label
    source_dir.mkdir()
    hashes = {}
    for name in ("syscall_bionic.cc", "posix_regex.h", "posix_regex_cnames.h"):
        data = read_source(name, revision)
        hashes[name] = sha(data)
        if instrument and name == "posix_regex.h":
            data = counted(data.decode()).encode()
        (source_dir / name).write_bytes(data)
    executable = source_dir / "regex-contracts"
    command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
              "-I", str(source_dir), "-I", str(ROOT / "compat/bionic"),
              "-I", str(ROOT / "compat/bionic/include"),
              "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "compat/elf/include")]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if objects:
        command += ["-DREGEX_ANDROID_REFERENCE"]
    if bounds:
        command += ["-DREGEX_WORK_BOUNDS"]
    command += [str(source_dir / "syscall_bionic.cc"),
                str(ROOT / "tests/native/regex_scaling_host.cc"), *objects, "-o", str(executable)]
    subprocess.run(command, check=True, timeout=150)
    print(label + " production hashes " + json.dumps(hashes, sort_keys=True), flush=True)
    return executable, hashes


def run(binary: Path, mode: str, expected_old_bounds: bool = False) -> tuple[str, list[dict]]:
    result = subprocess.run([str(binary), mode], text=True, capture_output=True, timeout=120)
    print(result.stdout, end="", flush=True)
    if expected_old_bounds:
        rejected = [line for line in result.stdout.splitlines() if line.startswith("FAIL ")]
        expected = [
            "FAIL one origin expands each NFA state at most once per input position",
            "FAIL one origin expands each NFA state at most once per input position",
            "FAIL boolean a-dot-star stops after its first accepted input byte",
            "FAIL boolean a-dot-star stops after its first accepted input byte",
        ]
        if (result.returncode != 1 or rejected != expected or
                "regex scaling: 38 checks, 4 failures" not in result.stdout):
            print(result.stderr, end="", flush=True)
            raise RuntimeError("historical work negative control did not fail in exactly four expected cases")
        print("Historical production rejected by all four new work controls", flush=True)
    elif result.returncode != 0:
        print(result.stderr, end="", flush=True)
        raise RuntimeError(f"{binary.name} {mode} failed: {result.returncode}")
    return result.stdout, [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
                           if line.startswith(("REGEX_WORK ", "REGEX_TIME "))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--compare-ref", help="Git revision for exact historical comparison")
    parser.add_argument("--android-reference", type=Path, metavar="CACHE",
                        help="download/verify pinned Android sources in this directory")
    parser.add_argument("--measure", action="store_true", help="five alternating uninstrumented CPU pairs")
    parser.add_argument("--output", type=Path, help="write complete source and measurement receipt")
    args = parser.parse_args()
    if args.measure and (args.sanitize or not args.compare_ref):
        parser.error("--measure requires --compare-ref and an unsanitized build")
    report = dict(time_utc=datetime.now(timezone.utc).isoformat(), platform=platform.platform(),
                  compiler=subprocess.check_output(shlex.split(args.cxx) + ["--version"], text=True),
                  baseline=args.compare_ref, sanitizer=args.sanitize, source_hashes={},
                  fixture_sha256=sha((ROOT / "tests/native/regex_scaling_host.cc").read_bytes()),
                  runner_sha256=sha(Path(__file__).read_bytes()),
                  android_reference=ANDROID_HASHES if args.android_reference else None,
                  records=[], checks={})
    with tempfile.TemporaryDirectory(prefix="gesher-regex-") as directory:
        folder = Path(directory)
        objects = (android_objects(folder, args.android_reference, args.cc, args.sanitize)
                   if args.android_reference else [])
        current, hashes = build(folder, "candidate", None, args, objects)
        report["source_hashes"]["candidate"] = hashes
        candidate_check, _ = run(current, "--check")
        report["checks"]["candidate"] = candidate_check
        instrumented, _ = build(folder, "candidate-counted", None, args, [], True, True)
        _, rows = run(instrumented, "--work")
        report["records"] += [dict(source="candidate", kind="work", **row) for row in rows]
        if args.compare_ref:
            baseline, hashes = build(folder, "baseline", args.compare_ref, args, objects)
            report["source_hashes"]["baseline"] = hashes
            baseline_check, _ = run(baseline, "--check")
            report["checks"]["baseline"] = baseline_check
            fingerprints = [next(line for line in s.splitlines() if line.startswith("REGEX_DIFFERENTIAL "))
                            for s in (candidate_check, baseline_check)]
            if fingerprints[0] != fingerprints[1]:
                raise RuntimeError("historical production differential mismatch")
            historical_control = subprocess.check_output(
                ["git", "rev-parse", args.compare_ref], cwd=ROOT, text=True).strip() == BASELINE
            baseline_counted, _ = build(folder, "baseline-counted", args.compare_ref, args, [],
                                        True, historical_control)
            _, rows = run(baseline_counted, "--work", historical_control)
            report["baseline_expected_work_failures"] = 4 if historical_control else 0
            report["records"] += [dict(source="baseline", kind="work", **row) for row in rows]
            if args.measure:
                for pair in range(5):
                    for label, executable in ([("baseline", baseline), ("candidate", current)]
                                              if pair % 2 == 0 else
                                              [("candidate", current), ("baseline", baseline)]):
                        _, rows = run(executable, "--measure")
                        report["records"] += [dict(source=label, kind="time", pair=pair, **row)
                                              for row in rows if "ns_per_call" in row]
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
