#!/usr/bin/env python3
"""Test production sendmsg control rejection with real host socket pairs.

The POSIX implementation, buffer helpers, bionic adapters and registrations
are extracted verbatim. The host envelope adapts guarded copies and payload
transport; the shared ELF fixture tests the full Windows path in native CI.
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


def between(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("production function boundaries changed: " + first)
    return source[source.index(first):source.index(following, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/posix/src/socket.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    wrappers = (ROOT / "compat/bionic/socket_bionic.cc").read_text(encoding="utf-8")
    helpers = between(source, "bool ToBuffers(", "}  // namespace\n\nextern \"C\" {\n\nssize_t send(")
    implementation = between(source, "ssize_t sendmsg(", "ssize_t recv(")
    adapters = between(wrappers, "int64_t BIONIC_SYSV Sendmsg(", "int64_t BIONIC_SYSV Recv(")
    adapters += between(wrappers, "int BIONIC_SYSV Sendmmsg(", "int BIONIC_SYSV Recvmmsg(")
    registrations = []
    for name in ("sendmsg", "sendmmsg"):
        matches = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', wrappers, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous public registration: " + name)
        registrations.extend(matches)
    template = (ROOT / "tests/native/socket_control_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in (("@SOCKET_HELPERS@", helpers),
                          ("@SENDMSG_IMPLEMENTATION@", implementation),
                          ("@SOCKET_ADAPTERS@", adapters),
                          ("@SOCKET_REGISTRATIONS@", "\n".join(registrations))):
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production socket.cc SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production sendmsg section SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-socket-control-") as temporary:
        folder = Path(temporary)
        generated = folder / "socket_control_test.cc"
        executable = folder / "socket_control_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-D_GNU_SOURCE",
            "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/posix/src"),
            "-I", str(ROOT / "compat/arm64/include"),
            "-I", str(ROOT / "tests/native"),
            str(generated), "-o", str(executable),
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
        if result.returncode:
            print(f"FAIL socket control: exit={result.returncode}\n{result.stderr[:12000]}", flush=True)
            return 1
        print("PASS socket control", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
