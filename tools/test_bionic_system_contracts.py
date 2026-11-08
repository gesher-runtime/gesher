#!/usr/bin/env python3
"""Exercise actual bionic getgroups/lockf adapters against host file-lock APIs.

The generated C++ contains the production functions and public registrations
verbatim, not an independent copy of their behavior. The shared fixture also
runs from ELF in the existing Windows x86-64 and ARM64 libc suites. Host success
checks the adapter contract; it does not verify Windows descriptor ownership.
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


def function_before(source: str, name: str, following: str) -> str:
    first = f"int BIONIC_SYSV {name}("
    last = f"int BIONIC_SYSV {following}("
    if source.count(first) != 1 or source.count(last) != 1:
        raise ValueError("production function boundaries changed: " + name)
    return source[source.index(first):source.index(last, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/extra_sys_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    implementation = function_before(source, "Lockf", "PosixFadvise") + "\n" + function_before(source, "Getgroups", "Setgroups")
    registrations = []
    for name in ("lockf", "lockf64", "getgroups"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous public registration: " + name)
        registrations.extend(matches)
    template = (ROOT / "tests/native/system_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in (("@SYSTEM_IMPLEMENTATION@", implementation),
                          ("@SYSTEM_REGISTRATIONS@", "\n".join(registrations))):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production extra_sys_bionic SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production adapter section SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-system-contracts-") as temporary:
        folder = Path(temporary)
        generated = folder / "system_contract_test.cc"
        executable = folder / "system_contract_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-D_GNU_SOURCE",
            "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"),
            str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["ASAN_OPTIONS"] = "allocator_may_return_null=1:" + env.get("ASAN_OPTIONS", "")
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("groups", "lockf"):
            result = subprocess.run([str(executable), case], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print(f"PASS {case}", flush=True)
        if failed:
            print("Failing system contracts: " + ", ".join(failed), flush=True)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
