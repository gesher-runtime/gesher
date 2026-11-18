#!/usr/bin/env python3
"""Execute valid original namespace/identity ELF fixtures through the full loader.

All production ELF translation units compile unchanged. Linux substitutes only
Win32 primitives and declares ARM unavailable. Each case has a fresh process;
the actual public libdl/ARM caller path has a separate production-wrapper gate.
--source-ref uses an old-API driver and tests distinct valid same-SONAME files,
so the historical control fails behaviorally rather than on a missing new API.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile

import test_elf_atomic_load as host

FIXTURES = (
    "elf_namespaces_test.cc", "elf_namespaces_legacy_control.cc",
    "elf_namespace_fixture.c", "build-elf-namespaces.sh",
    "elf_loader_host_platform.cc", "elf_loader_host_arm64.cc",
)
CASES = ("identity", "memory", "search", "scope", "groups", "links", "providers", "concurrent")


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
    with tempfile.TemporaryDirectory(prefix="gesher-elf-namespaces-") as temporary:
        folder = Path(temporary)
        elf, fixtures = host.snapshot(folder, args.source_ref, FIXTURES)
        includes = ["-I", str(fixtures / "elf_loader_host_platform"),
                    "-I", str(elf / "include"), "-I", str(elf),
                    "-I", str(fixtures / "arm64-include")]
        print("COMPILE_FLAGS " + json.dumps(flags + includes), flush=True)
        print("LINK_FLAGS " + json.dumps(link_flags), flush=True)
        compiler = folder / "cc-wrapper"
        compiler.write_text("#!/bin/sh\nexec " + shlex.join(cc) + ' "$@"\n', encoding="utf-8")
        compiler.chmod(0o700)
        graph = folder / "graph"
        host.run(["bash", str(fixtures / "build-elf-namespaces.sh"), str(compiler), str(graph)],
                 "BUILD original valid namespace ELF fixtures")
        sources = [elf / name for name in host.PRODUCTION_UNITS]
        sources += [fixtures / "elf_loader_host_platform.cc", fixtures / "elf_loader_host_arm64.cc",
                    fixtures / ("elf_namespaces_legacy_control.cc" if args.source_ref else "elf_namespaces_test.cc")]
        objects = []
        for index, path in enumerate(sources):
            target = folder / (str(index) + ".o")
            host.run(cxx + flags + includes + ["-c", str(path), "-o", str(target)], "COMPILE " + path.name)
            objects.append(str(target))
        executable = folder / "elf-namespaces"
        host.run(cxx + objects + link_flags + ["-o", str(executable)], "LINK complete production namespace runner")
        if args.source_ref:
            host.run([str(executable), str(graph)], "RUN historical file-identity behavior control", env=environment)
        else:
            for case in CASES:
                host.run([str(executable), str(graph), case], "RUN namespace contract " + case, env=environment)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
