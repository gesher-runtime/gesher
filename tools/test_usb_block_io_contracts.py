#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Execute the actual USB block POSIX adapter with an isolated transport boundary.

The source body and wire limit are extracted from production. Only the block
client/session boundary is modeled; this suite cannot prove Windows pipe,
authority, disk-handle or physical-media behavior.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def source_at(path, revision):
    if revision:
        return subprocess.check_output(["git", "show", revision + ":" + path], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def between(source, first, last):
    if source.count(first) != 1:
        raise ValueError("USB block source boundary changed: " + first)
    start = source.index(first)
    end = source.index(last, start)
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile an exact git revision as a negative control")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = source_at("compat/posix/src/usb_block.cc", args.source_ref)
    protocol = source_at("compat/usb_storage/block_broker.h", args.source_ref)
    error = between(source, "int BlockError(DWORD error) {", "\nbool PartitionNumber(")
    io = between(source,
        "bool IoLocal(const DescriptorSnapshotPtr& snapshot, void* data, unsigned size,\n        bool writing,",
        "\nbool SeekLocal(")
    constants = re.findall(r"^constexpr uint32_t kBlockTransferLimit = [^;]+;", protocol, re.MULTILINE)
    if len(constants) != 1:
        raise ValueError("USB block wire-limit declaration changed")
    template = (ROOT / "tests/native/usb_block_io_host_test.cc.in").read_text()
    for marker, replacement in (("@BLOCK_ERROR@", error), ("@IO_LOCAL@", io),
                                ("@TRANSFER_LIMIT@", constants[0])):
        if template.count(marker) != 1:
            raise ValueError("USB block host-template boundary changed: " + marker)
        template = template.replace(marker, replacement)
    print("Production usb_block.cc SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production IoLocal SHA256 " + hashlib.sha256(io.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-usb-block-io-") as temporary:
        folder = Path(temporary)
        generated = folder / "usb-block-io.cc"
        generated.write_text(template)
        executable = folder / "usb-block-io"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-pthread", str(generated), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        failures = []
        for case in ("large", "partial", "revocation", "bounds", "concurrent"):
            result = subprocess.run([str(executable), case], env=environment, cwd=folder,
                                    capture_output=True, text=True, timeout=45)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failures.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:6000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
