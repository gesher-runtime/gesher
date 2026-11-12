#!/usr/bin/env python3
"""Exercise verbatim sendfile and its public bionic bridge with real file/socket I/O.

Only I/O boundaries are substituted for deterministic failures, zero writes, and
the transfer cap. The shared C fixture also runs against the Linux reference.
This does not replace native Windows descriptor/socket integration coverage.
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
CASES = ("public", "sockets", "faults", "zero-write", "large")


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production section boundaries changed: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/posix/src/misc.cc")
    parser.add_argument("--source-ref", help="Read sendfile from this git revision for a negative control")
    parser.add_argument("--bionic-source", type=Path, default=ROOT / "compat/bionic/posix_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", action="append", choices=CASES)
    parser.add_argument("--skip-linux-reference", action="store_true")
    args = parser.parse_args()
    if args.source_ref:
        source = subprocess.run(
            ["git", "show", args.source_ref + ":compat/posix/src/misc.cc"], cwd=ROOT,
            check=True, text=True, stdout=subprocess.PIPE).stdout
    else:
        source = args.source.read_text(encoding="utf-8")
    bionic = args.bionic_source.read_text(encoding="utf-8")
    registrations = []
    for name in ("sendfile", "sendfile64"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', bionic, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous public registration: " + name)
        registrations.extend(matches)
    replacements = {
        "@SENDFILE_IMPLEMENTATION@": section(source, "ssize_t sendfile(", "// syslog:"),
        "@BIONIC_BRIDGE@": section(bionic, "int64_t BIONIC_SYSV Sendfile(", "// bionic's struct statvfs"),
        "@BIONIC_REGISTRATIONS@": "\n".join(registrations),
    }
    template = (ROOT / "tests/native/sendfile_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in replacements.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
        print(marker.strip("@") + " SHA256 " + hashlib.sha256(value.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-sendfile-") as temporary:
        folder = Path(temporary)
        generated = folder / "sendfile_test.cc"
        executable = folder / "sendfile_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64",
            "-I", str(ROOT / "compat/bionic"), "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        for case in args.case or CASES:
            result = subprocess.run([str(executable), case], text=True, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        if not args.skip_linux_reference:
            reference = folder / "sendfile_linux_reference.c"
            reference.write_text("""
#include <stdio.h>
static int assertions, failures;
static void check(const char* name, int ok) {
  ++assertions;
  if (!ok) { ++failures; printf("FAIL %s\\n", name); }
}
#include "sendfile_contract_test.inc"
int main(void) {
  test_sendfile_contracts();
  printf("linux-reference: %d assertions, %d failures\\n", assertions, failures);
  return failures ? 1 : 0;
}
""", encoding="utf-8")
            reference_exe = folder / "sendfile_linux_reference"
            command = shlex.split(args.cc) + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                "-D_GNU_SOURCE", "-D_FILE_OFFSET_BITS=64", "-I", str(ROOT / "tests/native"),
                str(reference), "-o", str(reference_exe),
            ]
            subprocess.run(command, check=True, timeout=60)
            result = subprocess.run([str(reference_exe)], text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append("linux-reference")
                print("FAIL linux-reference\n" + result.stderr[:12000], flush=True)
            else:
                print("PASS linux-reference", flush=True)
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
