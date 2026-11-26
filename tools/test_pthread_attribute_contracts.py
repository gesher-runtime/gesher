#!/usr/bin/env python3
"""Run verbatim bionic thread/attribute logic against a real Linux pthread boundary.

Windows stack/handle behavior is separately exercised by the mandatory native
target fixture. --source-ref runs the same safe assertions against old code.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source: str, first: str, last: str) -> str:
    if source.count(first) != 1 or source.count(last) != 1:
        raise ValueError("ambiguous production boundary: " + first)
    start = source.index(first)
    return source[start:source.index(last, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                           cwd=ROOT).decode()
        return (ROOT / path).read_text()

    pthread = read("compat/bionic/pthread_bionic.cc")
    extra = read("compat/bionic/extra_sys_bionic.cc")
    guest = read("compat/arm64/dll/guest_libc.cc")
    translator = read("compat/arm64/dll/translator.cc")
    parts = {
        "@KEY_STATE@": section(pthread, "constexpr int kKeysMax", "// --- threads"),
        "@THREAD_STATE@": section(pthread, "constexpr uint32_t kAttrDetached", "// --- mutex"),
        "@THREAD_API@": section(pthread, "// Threads.\n", "// Mutex attributes:"),
        "@ATTR_EXTRAS@": section(extra, "int BIONIC_SYSV PthreadAttrGetschedpolicy(",
                                "int BIONIC_SYSV PthreadGetnameNp("),
        "@THREAD_REGISTRATIONS@": "\n".join(re.findall(
            r"^  P\(pthread_(?:attr_[A-Za-z_]+|getattr_np|self|create|join|detach)\);$", pthread, re.M)),
        "@EXTRA_REGISTRATIONS@": "\n".join(re.findall(
            r'^  E\("pthread_attr_[A-Za-z_]+", PthreadAttr[A-Za-z_]+\);$', extra, re.M)),
        # The guest's thread start state ends where its fork section begins
        # (ADR 0092 replaced the EAGAIN stub's comment).
        "@GUEST_START_STATE@": section(guest, "using ThreadStart =",
                                       "// --- fork, vfork, clone" if "// --- fork, vfork, clone" in guest
                                       else "// fork()/vfork()"),
        "@GUEST_START_API@": section(guest, "void* GESHER_SYSV StartGuestThread(",
                                    "// arm64's wchar_t"),
        "@GUEST_STACK_PUBLICATION@": "" if args.source_ref else section(
            translator, "    // This private guest allocation differs", "    if (ProfilePeriodMs() > 0) {"),
    }
    template = (ROOT / "tests/native/pthread_attribute_contract_host_test.cc.in").read_text()
    for marker, code in parts.items():
        if template.count(marker) != 1:
            raise ValueError("ambiguous template marker: " + marker)
        template = template.replace(marker, code)
    for name, source in (("pthread", pthread), ("extra_sys", extra),
                         ("guest_libc", guest), ("translator", translator)):
        print(name + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-pthread-attributes-") as temporary:
        folder = Path(temporary)
        generated = folder / "contracts.cc"
        binary = folder / "contracts"
        generated.write_text(template)
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-Wno-unused-function", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-I" + str(ROOT / "compat/bionic"), "-I" + str(ROOT / "compat/arm64/include"),
            "-I" + str(ROOT / "tests/native"), str(generated), "-o", str(binary),
        ]
        if args.source_ref:
            command += ["-DGESHER_ATTRIBUTE_BASELINE"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], timeout=45).returncode


if __name__ == "__main__":
    raise SystemExit(main())
