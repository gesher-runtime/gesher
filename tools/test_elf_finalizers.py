#!/usr/bin/env python3
"""Execute complete production Close and lookup with owned lifecycle fixtures.

The Linux Win32 boundary is explicit. ARM Call is an observing boundary only;
actual translated finalizers remain a Windows runtime gate. --source-ref uses
the complete historical ELF tree, preserving behavioral lifecycle controls.
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

FIXTURES = ("elf_finalizers_host.cc", "elf_loader_host_platform.cc", "elf_loader_host_arm64.cc")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--source", type=Path, help="override only elf_loader.cc for an existing source control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="compile and test the complete historical production tree")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("the explicit host boundary requires Linux x86-64")
    cxx = shlex.split(args.cxx)
    if not cxx:
        parser.error("compiler commands must not be empty")
    print("BOUNDARY complete production ELF tree; Linux Win32 primitives; ARM Call observed, not executed", flush=True)
    for label, compiler in (("CXX", cxx),):
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
    with tempfile.TemporaryDirectory(prefix="gesher-elf-finalizers-") as temporary:
        folder = Path(temporary)
        elf, fixtures = host.snapshot(folder, args.source_ref, FIXTURES)
        includes = ["-I", str(fixtures / "elf_loader_host_platform"),
                    "-I", str(elf / "include"), "-I", str(elf),
                    "-I", str(fixtures / "arm64-include")]
        print("COMPILE_FLAGS " + json.dumps(flags + includes), flush=True)
        print("LINK_FLAGS " + json.dumps(link_flags), flush=True)
        if args.source:
            (elf / "elf_loader.cc").write_bytes(args.source.read_bytes())
            print("OVERRIDE_ELF_LOADER_SHA256 " + host.digest(elf / "elf_loader.cc"), flush=True)
        namespaced = "ScopedLoaderOperation::ScopedLoaderOperation" in (elf / "elf_loader.cc").read_text()
        flags += ["-DGESHER_HAS_NAMESPACES=" + str(int(namespaced))]
        sources = [elf / name for name in host.PRODUCTION_UNITS if name != "elf_loader.cc"]
        sources += [fixtures / "elf_loader_host_platform.cc", fixtures / "elf_loader_host_arm64.cc",
                    fixtures / "elf_finalizers_host.cc"]
        objects = []
        for index, path in enumerate(sources):
            target = folder / (str(index) + ".o")
            extra = ["-DCall=UnavailableArmCall"] if path.name == "elf_loader_host_arm64.cc" else []
            host.run(cxx + flags + includes + extra + ["-c", str(path), "-o", str(target)], "COMPILE " + path.name)
            objects.append(str(target))
        executable = folder / "elf-finalizers"
        host.run(cxx + objects + link_flags + ["-o", str(executable)], "LINK complete production lifecycle runner")
        host.run([str(executable)], "RUN actual production finalizer and lookup contracts", env=environment)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
