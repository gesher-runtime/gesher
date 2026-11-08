#!/usr/bin/env python3
"""Run production bionic directory APIs with real host directories and faults.

The directory section and public registrations are compiled verbatim. Host
readdir/closedir wrappers inject I/O errors, and linker allocation wrappers
exercise every copy/list allocation failure without consuming host memory.
Windows path mapping and enumeration remain the native ELF integration gate.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/posix_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    start = "struct BionicDirent {"
    end = "// --- the *at() calls and other path calls"
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("production directory section boundaries changed")
    implementation = source[source.index(start):source.index(end)]
    registrations = []
    for name in ("scandir", "scandir64", "scandirat", "scandirat64", "opendir", "readdir",
                 "closedir", "rewinddir", "telldir", "seekdir"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        if len(matches) > 1:
            raise ValueError("ambiguous public registration: " + name)
        registrations.extend(matches)
    template = (ROOT / "tests/native/directory_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in (("@DIRECTORY_IMPLEMENTATION@", implementation),
                          ("@DIRECTORY_REGISTRATIONS@", "\n".join(registrations))):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production posix_bionic SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production directory section SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-directories-") as temporary:
        folder = Path(temporary)
        generated = folder / "directory_test.cc"
        executable = folder / "directory_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-fno-builtin", "-pthread",
            "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"), str(generated),
            "-Wl,--wrap=malloc,--wrap=realloc,--wrap=free,--wrap=readdir,--wrap=closedir",
            "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["ASAN_OPTIONS"] = "allocator_may_return_null=1:" + env.get("ASAN_OPTIONS", "")
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("public", "allocations", "reader", "fdopen"):
            result = subprocess.run([str(executable), case], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print(f"PASS {case}", flush=True)
        if failed:
            print("Failing directory contracts: " + ", ".join(failed), flush=True)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
