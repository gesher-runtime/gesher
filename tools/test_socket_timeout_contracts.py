#!/usr/bin/env python3
"""Exercise production LP64 socket timeout conversion over real local sockets.

The guest adapters and host timeout branches are compiled verbatim with a
32-bit intermediate timeval. This catches narrowing hidden by Linux's long.
--linux-reference also runs the shared public contract directly against libc;
it does not include Gesher's documented negative-second or saturation limits.
Windows descriptor ownership and Winsock remain the native ELF suite's job.
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
        raise ValueError("production timeout boundaries changed: " + first)
    return source[source.index(first):source.index(following, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/socket_bionic.cc")
    parser.add_argument("--host-source", type=Path, default=ROOT / "compat/posix/src/socket.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--linux-reference", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    host = args.host_source.read_text(encoding="utf-8")
    host_set = section(host, "int setsockopt(", "int getsockopt(")
    host_get = host[host.index("int getsockopt("):]
    # The descriptor transfer functions (ADR 0088) follow the public API.
    transfer = "bool gesher::TransferSocket("
    if transfer in host_get:
        host_get = host_get[:host_get.index(transfer)]
    timeout_start = "  if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {"
    timeout_end = "  if (level == SOL_SOCKET && name == SO_LINGER) {"
    parts = {
        "@TIMEVAL_TYPE@": section(source, "struct BionicTimeval {", "// Pinned bionic's"),
        "@TIMEOUT_ADAPTERS@": section(source, "bool IsTimeoutOption(", "int64_t BIONIC_SYSV Send("),
        "@HOST_SET_PROLOGUE@": section(host_set, "  SocketFd state;", "  ws::Option option;"),
        "@HOST_GET_PROLOGUE@": section(host_get, "  SocketFd state;", "  ws::Option option;"),
        "@HOST_SET_TIMEOUT@": section(host_set, timeout_start, timeout_end),
        "@HOST_GET_TIMEOUT@": section(host_get, timeout_start, timeout_end),
    }
    registrations = []
    for name in ("setsockopt", "getsockopt"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous public registration: " + name)
        registrations.extend(matches)
    parts["@TIMEOUT_REGISTRATIONS@"] = "\n".join(registrations)
    template = (ROOT / "tests/native/socket_timeout_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in parts.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production socket adapter SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production host socket SHA256 " + hashlib.sha256(host.encode()).hexdigest(), flush=True)
    sources = [("production", template)]
    if args.linux_reference:
        sources.append(("linux-reference", '''#include <cstdio>
static int failures;
static void check(const char* what, bool ok) {
  std::printf("%-78s %s\\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}
#include "socket_timeout_contract_test.inc"
int main() { test_socket_timeout_contracts(); return failures != 0; }
'''))
    with tempfile.TemporaryDirectory(prefix="gesher-timeout-contracts-") as temporary:
        folder = Path(temporary)
        failed = []
        for label, text in sources:
            generated, executable = folder / (label + ".cc"), folder / label
            generated.write_text(text, encoding="utf-8")
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
                "-fno-builtin", "-I", str(ROOT / "compat/bionic"),
                "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "tests/native"),
                str(generated), "-ldl", "-o", str(executable),
            ]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            env = os.environ.copy()
            if args.sanitize:
                env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
            result = subprocess.run([str(executable)], env=env, cwd=folder, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            print(result.stdout, end="", flush=True)
            print(("FAIL " if result.returncode else "PASS ") + label, flush=True)
            if result.returncode:
                failed.append(label)
                print(result.stderr[:12000], flush=True)
        return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
