#!/usr/bin/env python3
"""Execute original ELF version fixtures through the complete production loader.

The Linux host boundary is shared with test_elf_atomic_load.py: only Win32
primitives and the unavailable ARM entry points are adapted. Parsing, symbol
matching, relocation, transactions, TLS and rewrite code compile unchanged.
Every case runs in a fresh process. Windows x86 and translated ARM execution
are separate native CI gates; this host run does not establish either.

--source-ref compiles the complete historical ELF tree and an old-API driver.
That driver tests a real hidden-only definition, so its failure is behavioral,
not an unavailable new API or a compile error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile

import test_elf_atomic_load as host
import test_elf_dlvsym_shim as shim

ROOT = Path(__file__).resolve().parents[1]

FIXTURES = (
    "elf_versions_test.cc", "elf_versions_legacy_control.cc",
    "elf_versions_fixture.c", "build-elf-versions.sh",
    "elf_guest_dlvsym_fixture.c", "elf_guest_dlvsym.map",
    "elf_null_reloc_fixture.c", "elf_dlvsym_null_host.cc.in",
    "elf_loader_host_platform.cc", "elf_loader_host_arm64.cc",
)
CASES = ("lookup-gnu", "lookup-sysv", "required", "weak", "weak-present",
         "unversioned", "global", "virtual", "tls", "tls-weak", "tls-weak-present")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="complete historical production tree; run old-API behavioral control")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("the explicit host boundary requires Linux x86-64")
    cxx, cc = shlex.split(args.cxx), shlex.split(args.cc)
    if not cxx or not cc:
        parser.error("compiler commands must not be empty")
    print("BOUNDARY complete production ELF tree; Linux x86-64 Win32 primitives; ARM unavailable", flush=True)
    for label, compiler in (("CXX", cxx), ("CC", cc)):
        version = subprocess.check_output(compiler + ["--version"], text=True).splitlines()[0]
        print(label + " " + json.dumps(compiler) + " " + version, flush=True)
    flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread"]
    link_flags = ["-pthread"]
    environment = os.environ.copy()
    if args.sanitize:
        sanitizer = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        flags += sanitizer + ["-fno-pie"]
        link_flags += sanitizer + ["-no-pie"]
        previous = environment.get("ASAN_OPTIONS", "")
        environment["ASAN_OPTIONS"] = previous + (":" if previous else "") + "detect_leaks=0"
        print("SANITIZERS address,undefined; fatal findings; detect_leaks=0 (ptrace boundary)", flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-elf-versions-") as temporary:
        folder = Path(temporary)
        elf, fixtures = host.snapshot(folder, args.source_ref, FIXTURES, ("elf_versions",))
        includes = ["-I", str(fixtures / "elf_loader_host_platform"),
                    "-I", str(elf / "include"), "-I", str(elf),
                    "-I", str(fixtures / "arm64-include")]
        print("COMPILE_FLAGS " + json.dumps(flags + includes), flush=True)
        print("LINK_FLAGS " + json.dumps(link_flags), flush=True)
        compiler = folder / "cc-wrapper"
        compiler.write_text("#!/bin/sh\nexec " + shlex.join(cc) + ' "$@"\n', encoding="utf-8")
        compiler.chmod(0o700)
        graph = folder / "graph"
        host.run(["bash", str(fixtures / "build-elf-versions.sh"), str(compiler), str(graph)],
                 "BUILD original valid versioned ELF fixtures")
        sources = [elf / name for name in host.PRODUCTION_UNITS]
        sources += [fixtures / "elf_loader_host_platform.cc", fixtures / "elf_loader_host_arm64.cc",
                    fixtures / ("elf_versions_legacy_control.cc" if args.source_ref else "elf_versions_test.cc")]
        objects = []
        for index, path in enumerate(sources):
            target = folder / (str(index) + ".o")
            host.run(cxx + flags + includes + ["-c", str(path), "-o", str(target)], "COMPILE " + path.name)
            objects.append(str(target))
        executable = folder / "elf-versions"
        host.run(cxx + objects + link_flags + ["-o", str(executable)], "LINK complete production version runner")
        if args.source_ref:
            host.run([str(executable), str(graph)], "RUN historical hidden-version behavior control", env=environment)
        else:
            for case in CASES:
                host.run([str(executable), str(graph), case], "RUN version contract " + case, env=environment)
            # Reuse the same complete production objects, replacing only the
            # driver with the actual shim dispatch/error/export section. This
            # covers successful null IFUNCs across the loader/shim boundary.
            body = (ROOT / "compat/bionic/shim.cc").read_text(encoding="utf-8")
            state, implementation, entries = shim.extract(body)
            template = (fixtures / "elf_dlvsym_null_host.cc.in").read_text(encoding="utf-8")
            dispatch = template.replace("@ERROR_STATE@", state).replace(
                "@LIBDL_IMPLEMENTATION@", implementation).replace("@LIBDL_ENTRIES@", entries)
            driver, driver_object = folder / "null-dispatch.cc", folder / "null-dispatch.o"
            driver.write_text(dispatch, encoding="utf-8")
            print("NULL_DISPATCH_SHA256 " + hashlib.sha256(dispatch.encode()).hexdigest(), flush=True)
            print("COMPLETE_SHIM_SHA256 " + hashlib.sha256(body.encode()).hexdigest(), flush=True)
            host.run(cxx + flags + includes + ["-c", str(driver), "-o", str(driver_object)],
                     "COMPILE actual shim null dispatch")
            null_runner = folder / "elf-null-dispatch"
            host.run(cxx + objects[:-1] + [str(driver_object)] + link_flags + ["-o", str(null_runner)],
                     "LINK actual shim with complete production ELF")
            host.run([str(null_runner), str(graph)], "RUN actual null IFUNC dispatch", env=environment)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
