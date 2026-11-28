#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Execute complete inotify source and its actual successful-write entry point.

The host boundary maps file handles and file identities to real Linux files;
Win32 completion delivery, clocks and the eventfd registry remain declared
adapters. Watch admission, routing, masks, record draining, retirement and reads
are production code. The public corpus also runs against real Linux inotify and
is included in Windows posix_probe. A host run is not Windows runtime evidence.
"""
from __future__ import annotations
import argparse
import hashlib
import os
import re
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("ambiguous production boundary: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


# Immutable main ancestor: the callback still requires DOS drive-name lookup.
# Unlike a PR head, this control survives rebases and squash merging.
BASELINE_REF = "e2685dc71e01122b25283ae17f5a2828bfc3f1f1"
DOS_DENIED_FAILURE = "FAIL directory identity NT route preserves immediate named modify (errno="


def baseline_negative(args: argparse.Namespace) -> bool:
    command = [sys.executable, str(Path(__file__).resolve()), "--source-ref", BASELINE_REF,
               "--cxx", args.cxx]
    if args.sanitize:
        command += ["--sanitize"]
    # Keep a small durable log even when generated translation units are temporary.
    directory = args.keep_dir or ROOT / "out/inotify-routing-controls"
    directory.mkdir(parents=True, exist_ok=True)
    log = directory / ("baseline-negative-sanitize.log" if args.sanitize else "baseline-negative.log")
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=180)
        output = result.stdout + result.stderr
        # A missing ancestor, compile failure, abort, or sanitizer failure cannot
        # establish the intended before/after behavior. Require the host corpus
        # to complete and fail at the explicit DOS-denied callback assertion.
        failed_roles = re.findall(r"(?m)^FAIL (reference|production-host): exit=([^\r\n]+)\r?$", output)
        reference = re.findall(r"(?m)^inotify routing Linux reference: [1-9]\d* checks, 0 failures\r?$", output)
        host = re.findall(r"(?m)^inotify routing production host: [1-9]\d* checks, [1-9]\d* failures\r?$", output)
        intended = re.search(r"(?m)^" + re.escape(DOS_DENIED_FAILURE) + r"\d+\)\r?$", output)
        passed = result.returncode == 1 and bool(intended) and len(reference) == len(host) == 1 and \
            failed_roles == [("production-host", "1")] and not any(
                marker in output for marker in ("ERROR: AddressSanitizer", "runtime error:",
                                                "LeakSanitizer:", "DEADLYSIGNAL", "Traceback (most recent call last):"))
    except subprocess.TimeoutExpired as error:
        def decoded(raw: str | bytes | None) -> str:
            return raw.decode(errors="replace") if isinstance(raw, bytes) else raw or ""
        output = decoded(error.stdout) + decoded(error.stderr) + "\ncontrol timed out\n"
        passed = False
    encoded = output.encode()
    bounded = encoded if len(encoded) <= 65536 else (
        encoded[:32000] + b"\n[log truncated]\n" + encoded[-32000:])
    log.write_bytes(bounded)
    print(("BASELINE_NEGATIVE_PASS" if passed else "FAIL baseline negative control") +
          f" source={BASELINE_REF} log={log}", flush=True)
    if not passed:
        print(output[-4000:], flush=True)
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--keep-dir", type=Path)
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                           cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    source = read("compat/posix/src/inotify.cc")
    fd = read("compat/posix/src/fd.cc")
    for name, data in (("inotify.cc", source), ("fd.cc", fd)):
        print(name + " SHA256 " + hashlib.sha256(data.encode()).hexdigest(), flush=True)
    for include in ('#include <windows.h>\n', '#include "event_internal.h"\n', '#include "internal.h"\n'):
        if source.count(include) != 1:
            raise ValueError("platform include changed: " + include)
        source = source.replace(include, "")
    # Optional only for --source-ref controls predating storage policy. The
    # fixture supplies an explicit granted boundary, like its Win32 adapters.
    policy_include = '#include "storage_policy.h"\n'
    if source.count(policy_include) > 1:
        raise ValueError("storage policy include is ambiguous")
    source = source.replace(policy_include, "")
    # The Linux host maps the NT held-directory reopen boundary to a fresh
    # open of the same Linux descriptor. Historic controls predate the helper.
    reopen_include = '#include <gesher/directory_reopen.h>\n'
    if source.count(reopen_include) != int("gesher::directory_reopen::Open(" in source):
        raise ValueError("directory reopen boundary changed")
    source = source.replace(reopen_include, "")
    host = (ROOT / "tests/native/inotify_write_routing_host.cc.in").read_text()
    parent_routing = 'int directory_wd = 0;' in source and 'SameIdentity(held_parent, parent_id)' in source
    if not args.source_ref and not parent_routing:
        raise ValueError("current source omitted held-parent identity routing")
    pieces = {
        "INOTIFY_SOURCE": source,
        "TRANSFER_FILE_HANDLE": section(fd, "int TransferFileHandle(", "// Capture the object and shared description"),
        "HAS_ROUTING_INDEX": str(int("std::map<FileIdentity, int> identities;" in source)),
        "HAS_PARENT_ID_ROUTING": str(int(parent_routing)),
    }
    for name, data in pieces.items():
        marker = "@" + name + "@"
        if host.count(marker) != 1:
            raise ValueError("ambiguous fixture marker: " + name)
        host = host.replace(marker, data)
    reference = '''#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
static int checked, failed;
static void route_check(const char* label, bool ok) {
  ++checked; if (!ok) { ++failed; std::printf("FAIL %s (errno=%d)\\n", label, errno); }
}
#include "inotify_write_routing_test.inc"
int main() {
  char directory[] = "/tmp/gesher-inotify-route-XXXXXX";
  if (!mkdtemp(directory)) return 2;
  InotifyWriteRouting(directory);
  route_check("reference temporary root retires", rmdir(directory) == 0);
  std::printf("inotify routing Linux reference: %d checks, %d failures\\n", checked, failed);
  return failed != 0;
}
'''
    failure = False
    with tempfile.TemporaryDirectory(prefix="gesher-inotify-routing-") as temporary:
        work = args.keep_dir or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        (work / "gesher").mkdir(exist_ok=True)
        (work / "gesher/reserved_names.h").write_text(read("compat/posix/include/gesher/reserved_names.h"))
        roles = (("production-host", host),) if args.measure else (("reference", reference), ("production-host", host))
        for role, content in roles:
            path, binary = work / (role + ".cc"), work / role
            path.write_text(content)
            command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-pthread", "-I", str(work), "-I", str(ROOT / "tests/native"), str(path), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
            subprocess.run(command, check=True, timeout=120)
            result = subprocess.run([str(binary)] + (["--measure"] if args.measure else []),
                                    capture_output=True, text=True, timeout=60)
            (work / (role + ".txt")).write_text(result.stdout + result.stderr)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failure = True
                print(f"FAIL {role}: exit={result.returncode}\n{result.stderr[:8000]}", flush=True)
    # Source controls never recurse, and measurement remains a single-source run.
    if not failure and not args.source_ref and not args.measure:
        failure = not baseline_negative(args)
    return int(failure)


if __name__ == "__main__":
    raise SystemExit(main())
