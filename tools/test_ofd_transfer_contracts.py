#!/usr/bin/env python3
"""Exercise production OFD ownership and exec transport with real Linux objects.

This compiles the exact ofd.cc implementation and SpawnWithFds builder. A
declared Win32/UCRT boundary uses real Linux descriptors, shared mmap sections,
and posix_spawn. It checks production aliasing, startup import, close/reuse,
cross-process status changes and cleanup. It does not prove Windows ABI,
CreateProcess/UCRT startup behavior, or native Windows append semantics; those
remain covered by the Windows native/ELF fixtures.
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


def without_includes(source):
    return re.sub(r"^#(?:include|pragma once)[^\n]*\n", "", source, flags=re.MULTILINE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="negative control for the exact production OFD implementation")
    args = parser.parse_args()
    public = (ROOT / "compat/posix/include/gesher/file_description.h").read_text()
    private = (ROOT / "compat/posix/src/ofd_internal.h").read_text()
    implementation = subprocess.check_output(["git", "show", args.source_ref +
        ":compat/posix/src/ofd.cc"], cwd=ROOT, text=True) if args.source_ref else (
        ROOT / "compat/posix/src/ofd.cc").read_text()
    spawn = (ROOT / "compat/elf/exec/exec_spawn.cc").read_text()
    begin, end = "DWORD SpawnWithFds(", "\nstd::wstring EnvironmentBlock("
    if spawn.count(begin) != 1 or spawn.count(end) != 1:
        raise ValueError("SpawnWithFds production boundaries changed")
    builder = spawn[spawn.index(begin):spawn.index(end)]
    template = (ROOT / "tests/native/ofd_transfer_host_test.cc.in").read_text()
    sections = {
        "@PE_EXPORTS_HEADER@": str(ROOT / "compat/elf/exec/pe_exports.h"),
        "@ENVIRONMENT_ENCODING_HEADER@": str(ROOT / "compat/posix/include/gesher/environment_utf16.h"),
        "@OFD_DECLARATIONS@": without_includes(public) + without_includes(private),
        "@OFD_IMPLEMENTATION@": without_includes(implementation),
        "@SPAWN_IMPLEMENTATION@": builder,
        "@ENVIRONMENT_BLOCK@": spawn[spawn.index("std::wstring EnvironmentBlock("):spawn.index("\nint ExitStatus(")],
    }
    for token, section in sections.items():
        if template.count(token) != 1:
            raise ValueError("OFD template boundary changed: " + token)
        template = template.replace(token, section)
    print("Production ofd.cc SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    print("Production SpawnWithFds SHA256 " + hashlib.sha256(builder.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-ofd-transfer-") as temporary:
        folder = Path(temporary)
        source = folder / "ofd_transfer.cc"
        source.write_text(template)
        executable = folder / "ofd_transfer"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fexceptions", "-D_GNU_SOURCE", "-pthread",
            str(source), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        return subprocess.run([str(executable)], cwd=folder, env=environment, timeout=60).returncode


if __name__ == "__main__":
    raise SystemExit(main())
