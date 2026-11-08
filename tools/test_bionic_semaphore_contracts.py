#!/usr/bin/env python3
"""Test actual semaphore/deadline code with Linux futexes and controlled clocks.

Production code and public registrations are copied verbatim. This envelope
does not exercise Windows WaitOnAddress, signal delivery or cross-process
semaphores. The shared public fixture also runs in the native ELF suites.
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


def section(source: str, first: str, last: str) -> str:
    if source.count(first) != 1 or source.count(last) != 1:
        raise ValueError("production section boundaries changed: " + first)
    return source[source.index(first):source.index(last, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/extra_sys_bionic.cc")
    parser.add_argument("--shim-source", type=Path, default=ROOT / "compat/bionic/shim.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    shim = args.shim_source.read_text(encoding="utf-8")
    implementation = section(source, "int64_t MsUntil(", "// --- scheduling")
    word = section(source, "std::atomic<uint32_t>* Word(", "int64_t NowRealtimeNs(")
    deadline = section(shim, "uint32_t MillisUntil(", "void BIONIC_SYSV GcovStub(")
    registrations = re.findall(r'^  E\("sem_[A-Za-z_]+", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
    if len(registrations) != 12:
        raise ValueError("public semaphore registration count changed")
    template = (ROOT / "tests/native/semaphore_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in (("@WORD_IMPLEMENTATION@", word),
                          ("@SEMAPHORE_IMPLEMENTATION@", implementation),
                          ("@DEADLINE_IMPLEMENTATION@", deadline),
                          ("@SEMAPHORE_REGISTRATIONS@", "\n".join(registrations))):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production semaphore source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production deadline source SHA256 " + hashlib.sha256(shim.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-semaphores-") as temporary:
        folder = Path(temporary)
        generated = folder / "semaphore_test.cc"
        executable = folder / "semaphore_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-fno-builtin", "-pthread",
            "-I", str(ROOT / "compat/bionic"), "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("limits", "waits", "deadlines"):
            result = subprocess.run([str(executable), case], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            else:
                print(f"PASS {case}", flush=True)
        if failed:
            print("Failing semaphore contracts: " + ", ".join(failed), flush=True)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
