#!/usr/bin/env python3
"""Run production bionic key APIs and exit rounds with real host pthread TLS.

The Windows thread-record boundary is modeled; key state, API registration,
cleanup order and host-exit arming are extracted verbatim. This does not replace
the Windows bionic_key_exit_test integration gate.
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
        raise ValueError("production key section boundaries changed: " + first)
    return source[source.index(first):source.index(following, source.index(first))]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/pthread_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    parts = {
        "@KEY_STATE_AND_EXIT@": section(source, "constexpr int kKeysMax", "// --- threads"),
        "@KEY_HOST_EXIT@": section(source, "pthread_key_t g_host_exit_key;", "struct StartArg"),
        "@KEY_API@": section(source, "int BIONIC_SYSV gesher_bionic_pthread_key_create(",
                            "int BIONIC_SYSV gesher_bionic_register_atfork("),
    }
    registrations = []
    for name in ("pthread_key_create", "pthread_key_delete", "pthread_getspecific", "pthread_setspecific"):
        found = re.findall(r'^  P\(' + name + r'\);$', source, re.M)
        if len(found) != 1:
            raise ValueError("missing or ambiguous public key registration: " + name)
        registrations += found
    parts["@KEY_REGISTRATIONS@"] = "\n".join(registrations)
    template = (ROOT / "tests/native/pthread_key_contract_host_test.cc.in").read_text(encoding="utf-8")
    for marker, value in parts.items():
        if template.count(marker) != 1:
            raise ValueError("missing or ambiguous template marker: " + marker)
        template = template.replace(marker, value)
    print("Production pthread source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-bionic-keys-") as temporary:
        folder = Path(temporary)
        generated = folder / "pthread_key_test.cc"
        executable = folder / "pthread_key_test"
        generated.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-fno-builtin", "-pthread", "-I", str(ROOT / "compat/bionic"),
            "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "tests/native"),
            str(generated), "-o", str(executable),
        ]
        if "uint64_t g_key_generations[kKeysMax]" in source:
            command += ["-DHAVE_KEY_GENERATIONS"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("public", "mutations", "exit_order", "host_exit", "generation_limit"):
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
