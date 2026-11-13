#!/usr/bin/env python3
"""Exercise production timestamp/guest filesystem code at declared Win32 boundaries.

The C++ harness models LLP64 timespec fields and observes Windows handle ownership,
flags, timestamp writes and errors. It does not replace the real filesystem fixture
in tests/native/filesystem_metadata_contract_test.inc, run on Windows and Linux.
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
GROUPS = ("conversion", "nofollow", "ordering", "descriptors", "guest", "delegation", "lengthen")


def between(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("Production timestamp region is missing or ambiguous")
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def function(source: str, name: str) -> str:
    marker = "int BIONIC_SYSV " + name + "("
    if source.count(marker) != 1:
        raise ValueError("Production filesystem function is missing or ambiguous: " + name)
    begin = source.index(marker)
    brace = source.index("{", begin)
    depth = 1
    at = brace + 1
    while depth:
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
        at += 1
    return source[begin:at]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Use old production source as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--group", action="append", choices=GROUPS)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    sources = {}
    for path in ("compat/posix/src/path.cc", "compat/bionic/posix_bionic.cc"):
        source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT, text=True)
                  if args.source_ref else (ROOT / path).read_text())
        print(path + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
        sources[path] = source
    host = between(sources["compat/posix/src/path.cc"], "// --- file times", "\nint mkdir(const char* path)")
    lengthen = between(sources["compat/posix/src/path.cc"], "void Lengthen(", "\nDWORD Access(")
    guest = sources["compat/bionic/posix_bionic.cc"]
    timestamp = between(guest, "bool NarrowTimes(", "\nchar* BIONIC_SYSV Getcwd(")
    filesystem = "\n\n".join(function(guest, name) for name in ("Remove", "Renameat", "Rename", "Chmod", "Fchmodat"))
    template = (ROOT / "tests/native/file_timestamp_host_test.cc.in").read_text()
    for marker, value in {"@HOST_TIMESTAMPS@": host, "@GUEST_TIMESTAMPS@": timestamp,
                          "@GUEST_FILESYSTEM@": filesystem, "@LENGTHEN@": lengthen}.items():
        if template.count(marker) != 1:
            raise ValueError("Missing or ambiguous harness marker " + marker)
        template = template.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix="gesher-file-timestamps-") as temporary:
        work = Path(temporary)
        source, binary = work / "contracts.cc", work / "contracts"
        source.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-function", str(source), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True, timeout=120)
        failed = False
        for group in args.group or GROUPS:
            result = subprocess.run([str(binary), group], timeout=30)
            failed |= result.returncode != 0
        return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
