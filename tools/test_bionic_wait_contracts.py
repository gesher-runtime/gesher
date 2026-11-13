#!/usr/bin/env python3
"""Test production poll/select adapters with pipes and controlled clock/wait boundaries.

The complete adapter section and export registrations are extracted verbatim.
Controlled waits exercise durations beyond poll's signed millisecond range;
real pipe tests exercise public contracts. Atomic signal masks remain unsupported.
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
        raise ValueError("production wait section boundaries changed: " + first)
    return source[source.index(first):source.index(following, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/socket_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    implementation = section(source, "int BIONIC_SYSV Poll(", "[[noreturn]] void FdSetOverflow(")
    timeval = section(source, "struct BionicTimeval {", "// Pinned bionic's")
    registrations = []
    for name in ("poll", "__poll_chk", "ppoll", "ppoll64", "__ppoll_chk", "__ppoll64_chk",
                 "select", "pselect", "pselect64"):
        found = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        if len(found) != 1:
            raise ValueError("missing or ambiguous public wait registration: " + name)
        registrations += found
    template = (ROOT / "tests/native/poll_timeout_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in (("@WAIT_IMPLEMENTATION@", implementation), ("@TIMEVAL@", timeval),
                          ("@WAIT_REGISTRATIONS@", "\n".join(registrations))):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production socket source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production wait section SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-bionic-waits-") as temporary:
        folder = Path(temporary)
        generated = folder / "poll_timeout_test.cc"
        executable = folder / "poll_timeout_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "tests/native"),
            str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("public", "boundaries", "budgets", "errors"):
            result = subprocess.run([str(executable), case], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
