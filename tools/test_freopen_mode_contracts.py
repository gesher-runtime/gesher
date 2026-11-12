#!/usr/bin/env python3
"""Check production freopen modes against actual file effects and Linux.

Extracts both production adapters. The host envelope supplies declared Windows
boundaries and maps the CRT's N option to Linux e; real libc performs the open,
exclusive-create decision, descriptor closure, byte I/O and inheritance flags.
The same C fixture also runs independently on Linux and in Android ELF tests.
This host test does not establish real Windows CRT or record-lock integration.

The Linux backend requires glibc 2.41 or newer for the CLOEXEC-reset and
failed-reopen allocation fixes (glibc bugs 32134 and 32140). CI runs this suite
unchanged through tools/ci/test-freopen-reference.sh in a pinned Debian image.
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


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production section boundaries changed: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-source", type=Path, default=ROOT / "compat/posix/src/fsops.cc")
    parser.add_argument("--guest-source", type=Path, default=ROOT / "compat/bionic/stdio_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    host = args.host_source.read_text(encoding="utf-8")
    guest = args.guest_source.read_text(encoding="utf-8")
    for symbol in ("freopen", "freopen64"):
        pattern = r'^  E\("' + symbol + r'", Freopen\);$'
        if len(re.findall(pattern, guest, re.M)) != 1:
            raise ValueError("missing or ambiguous guest registration: " + symbol)
    guest_reopen = section(guest, "FILE* BIONIC_SYSV Freopen(", "int BIONIC_SYSV Fgetpos(")
    if guest_reopen.count("std::freopen(") != 1:
        raise ValueError("guest native reopen boundary changed")
    sections = {
        "@HOST_PARSE_MODE@": section(host, "bool ParseMode(", "// Random characters"),
        "@HOST_FREOPEN@": section(host, "FILE* freopen(", "char* mkdtemp("),
        "@GUEST_CRT_MODE@": section(guest, "std::string CrtMode(", "}  // namespace\n\n// /dev/zero"),
        "@GUEST_FREOPEN@": guest_reopen,
    }
    template = (ROOT / "tests/native/freopen_mode_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in sections.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        print(marker.strip("@") + " SHA256 " + hashlib.sha256(value.encode()).hexdigest(), flush=True)
        if marker == "@GUEST_FREOPEN@":
            # Route the production call into the production host adapter in
            # this executable, rather than bypassing it via Linux's libc.
            value = value.replace("std::freopen(", "HostFreopen(")
        if marker == "@HOST_FREOPEN@":
            # Production defines these C entry points in the global scope.
            # Qualify the unchanged alias target after nesting them here, so
            # FILE's argument-dependent lookup cannot select Linux freopen.
            alias = "return freopen(path, mode, stream);"
            if value.count(alias) != 1:
                raise ValueError("host freopen64 alias changed")
            value = value.replace(alias, "return host::freopen(path, mode, stream);")
        template = template.replace(marker, value)
    failed = []
    with tempfile.TemporaryDirectory(prefix="gesher-freopen-") as temporary:
        folder = Path(temporary)
        generated = folder / "freopen_mode_test.cc"
        executable = folder / "freopen_mode_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
            "-D_GNU_SOURCE", "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        for case in ("host", "guest"):
            result = subprocess.run([str(executable), case], cwd=folder, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print("FAIL " + case + "\n" + result.stderr[:12000], flush=True)
            else:
                print("PASS " + case, flush=True)
        reference = folder / "freopen_linux_reference.c"
        reference.write_text("""
#include <stdio.h>
static int failures, checks;
static void check(const char* name, int ok) {
  ++checks;
  if (!ok) { ++failures; fprintf(stderr, "FAIL: %s\\n", name); }
}
#include "freopen_mode_contract_test.inc"
int main(void) {
  test_freopen_mode_contracts();
  printf("freopen Linux: %d checks, %d failures\\n", checks, failures);
  return failures ? 1 : 0;
}
""", encoding="utf-8")
        reference_exe = folder / "freopen_linux_reference"
        subprocess.run(shlex.split(args.cc) + [
            "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
            "-D_GNU_SOURCE", "-I", str(ROOT / "tests/native"), str(reference), "-o", str(reference_exe),
        ], check=True, timeout=60)
        result = subprocess.run([str(reference_exe)], cwd=folder, text=True,
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
