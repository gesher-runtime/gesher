#!/usr/bin/env python3
"""Exercise production statvfs/fstatvfs adapters and guest aliases.

Extracted host functions use observed Windows path, handle, and volume
boundaries. A separate public C fixture operates on real Linux files and is
included by both Android ELF fixtures for required Windows execution.
The host model does not establish real drive/UNC permissions or integration.
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
CASES = ("ordinary", "namespaces", "fallback", "errors")


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production section boundaries changed: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-source", type=Path, default=ROOT / "compat/posix/src/system.cc")
    parser.add_argument("--guest-source", type=Path, default=ROOT / "compat/bionic/posix_bionic.cc")
    parser.add_argument("--header", type=Path, default=ROOT / "compat/posix/include/sys/statvfs.h")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--skip-linux-reference", action="store_true")
    args = parser.parse_args()
    host = args.host_source.read_text(encoding="utf-8")
    guest = args.guest_source.read_text(encoding="utf-8")
    header = args.header.read_text(encoding="utf-8")
    registrations = []
    for name in ("statvfs", "statvfs64", "fstatvfs", "fstatvfs64"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', guest, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous public registration: " + name)
        registrations.extend(matches)
    replacements = {
        "@HOST_TYPES@": section(header, "typedef unsigned long long fsblkcnt_t;", "#define ST_RDONLY"),
        "@FILL_IMPLEMENTATION@": section(host, "void FillStatvfs(", '\n}  // namespace\n\nextern "C"'),
        "@HOST_IMPLEMENTATION@": section(host, "// --- statvfs ---", "// --- mount ---"),
        "@GUEST_IMPLEMENTATION@": section(guest, "// bionic's struct statvfs:", "// pathconf/fpathconf"),
        "@PUBLIC_REGISTRATIONS@": "\n".join(registrations),
    }
    template = (ROOT / "tests/native/statvfs_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in replacements.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
        print(marker.strip("@") + " SHA256 " + hashlib.sha256(value.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-statvfs-") as temporary:
        folder = Path(temporary)
        generated = folder / "statvfs_host_test.cc"
        executable = folder / "statvfs_host_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"), str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        for case in args.case or CASES:
            result = subprocess.run([str(executable), case], text=True, env=env, cwd=folder,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        if not args.skip_linux_reference:
            reference = folder / "statvfs_linux_reference.c"
            reference.write_text("""
#include <stdio.h>
static int failures, assertions;
static void check(const char* name, int ok) {
  printf("%-88s %s\\n", name, ok ? "ok" : "FAIL");
  ++assertions;
  if (!ok) ++failures;
}
#include "statvfs_contract_test.inc"
int main(void) {
  test_statvfs_contracts();
  printf("statvfs Linux reference: %d assertions, %d failures\\n", assertions, failures);
  return failures ? 1 : 0;
}
""", encoding="utf-8")
            reference_exe = folder / "statvfs_linux_reference"
            command = shlex.split(args.cc) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                "-D_GNU_SOURCE", "-I", str(ROOT / "tests/native"),
                str(reference), "-o", str(reference_exe),
            ]
            subprocess.run(command, check=True, timeout=60)
            result = subprocess.run([str(reference_exe)], text=True, cwd=folder,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append("linux-reference")
                print("FAIL linux-reference\n" + result.stderr[:12000], flush=True)
            else:
                print("PASS linux-reference", flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
