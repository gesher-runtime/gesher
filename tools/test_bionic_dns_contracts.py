#!/usr/bin/env python3
"""Test production guest addrinfo conversion with numeric lookups and allocation failures.

The converters, free routine and public registrations are extracted verbatim.
Synthetic backend lists expose ownership and deep-copy behavior without DNS or
external networking; the public fixture also resolves a numeric loopback address.
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
        raise ValueError("production DNS section boundaries changed: " + first)
    return source[source.index(first):source.index(following, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/socket_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    parts = {
        "@DNS_TYPES_AND_FLAGS@": section(source, "struct BionicAddrinfo {", "// bionic NI_*"),
        "@DNS_CONVERTER@": section(source, "int GetaddrinfoOnInterface(", "bool NetworkIndex("),
        "@DNS_NETWORK_INDEX@": section(source, "bool NetworkIndex(", "int BIONIC_SYSV SetProcNetwork("),
        "@DNS_NETWORK_WRAPPER@": section(source, "int BIONIC_SYSV GetaddrinfoForNetwork(", "int BIONIC_SYSV ResNquery("),
        "@DNS_FREE@": section(source, "void BIONIC_SYSV Freeaddrinfo(BionicAddrinfo* a) {", "const char* BIONIC_SYSV GaiStrerror("),
    }
    registrations = []
    for name in ("getaddrinfo", "android_getaddrinfofornetwork", "freeaddrinfo"):
        found = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        if len(found) != 1:
            raise ValueError("missing or ambiguous public DNS registration: " + name)
        registrations += found
    parts["@DNS_REGISTRATIONS@"] = "\n".join(registrations)
    template = (ROOT / "tests/native/addrinfo_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in parts.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production socket source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Production DNS converter SHA256 " + hashlib.sha256(parts["@DNS_CONVERTER@"].encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-bionic-dns-") as temporary:
        folder = Path(temporary)
        generated = folder / "addrinfo_test.cc"
        executable = folder / "addrinfo_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-D_GNU_SOURCE", "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "tests/native"),
            str(generated), "-Wl,--wrap=calloc", "-Wl,--wrap=free", "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("public", "allocation", "backend_errors"):
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
