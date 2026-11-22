#!/usr/bin/env python3
"""Exercise production Binder delivery with declared pipe and Win32 boundaries.

Uses the actual WriteRead, wire readers, SG/FD metadata validators, mapped
allocator, FD delivery and guarded publication code. The fixture supplies
deterministic pipe, process-memory and descriptor-ownership adapters; native
Windows OFD/broker/ART gates remain separate. --source-ref runs the same behavior
and resource suite against an earlier source, with its declared wire format.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(text: str, start: str, end: str) -> str:
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("Binder receive source boundary changed")
    return text[text.index(start):text.index(end)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--measure", action="store_true",
                        help="run behavior checks and print measurements without new resource budgets")
    args = parser.parse_args()

    def read(relative: str) -> bytes:
        if args.source_ref:
            return subprocess.check_output(["git", "show", f"{args.source_ref}:{relative}"], cwd=ROOT)
        return (ROOT / relative).read_bytes()

    source = read("compat/posix/src/binder.cc")
    sg = read("compat/posix/src/binder_sg.h")
    wire = read("compat/posix/src/binder_wire.h")
    shared_descriptions = b"struct FileDescription {" in wire
    descriptions = read("compat/posix/src/binder_file_descriptions.h") if shared_descriptions else None
    print("Binder client SHA256 " + hashlib.sha256(source).hexdigest(), flush=True)
    print("Binder SG SHA256 " + hashlib.sha256(sg).hexdigest(), flush=True)
    print("Binder wire SHA256 " + hashlib.sha256(wire).hexdigest(), flush=True)
    if descriptions is not None:
        print("Binder FD descriptions SHA256 " + hashlib.sha256(descriptions).hexdigest(), flush=True)
    text = source.decode("utf-8")
    substitutions = {
        "@SHARED_DESCRIPTIONS@": str(int(shared_descriptions)),
        "@SOCKET_DESCRIPTIONS@": str(int(b"kSocketFileDescription" in wire)),
        "@PIPE@": section(text, "bool ReadExact(HANDLE pipe,", "std::wstring PipeName() {"),
        "@ALLOCATION@": section(text, "struct Allocation {", "std::mutex files_mutex;"),
        "@FD@": section(text, "template <typename F>\nvoid ForEachFdObject", "// The uid the driver gives"),
        "@DELIVERY@": section(text, "bool IsTransactionCommand(uint32_t cmd)", "// An ioctl the broker answers:"),
    }
    fixture = (ROOT / "tests/binder/receive_views_host.cc.in").read_text(encoding="utf-8")
    for marker, replacement in substitutions.items():
        if fixture.count(marker) != 1:
            raise ValueError("Binder fixture marker changed: " + marker)
        fixture = fixture.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="gesher-binder-receive-") as directory:
        work = Path(directory)
        (work / "binder_sg.h").write_bytes(sg)
        (work / "binder_wire.h").write_bytes(wire)
        if descriptions is not None:
            (work / "binder_file_descriptions.h").write_bytes(descriptions)
        cpp = work / "receive.cc"
        cpp.write_text(fixture, encoding="utf-8")
        binary = work / "receive"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-pthread", "-fno-builtin-memcpy", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary), "measure" if args.measure else "gate"],
                              check=False, timeout=90).returncode


if __name__ == "__main__":
    raise SystemExit(main())
