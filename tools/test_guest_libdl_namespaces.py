#!/usr/bin/env python3
"""Execute real guest libdl callers through the production wrappers and loader.

The Linux host boundary adapts Win32 primitives only. The complete common ELF
translation units and the public/internal libdl section compile unchanged.
Provider callbacks are explicitly isolated fixture libraries. --shim-source-ref
keeps the current loader and runs historical public wrappers as a route control.
This host test does not establish Windows or native Windows ARM execution.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

import test_elf_atomic_load as host

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = (
    "guest_libdl_namespace_host.cc.in", "guest_libdl_namespace_test.cc",
    "guest_libdl_namespace_arm64.cc.in",
    "elf_namespace_fixture.c", "build-elf-namespaces.sh",
    "elf_loader_host_platform.cc", "elf_loader_host_arm64.cc",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--fixtures", type=Path, help="existing original compiler-built fixture directory")
    parser.add_argument("--driver", type=Path, help="focused local integration driver using this same explicit boundary")
    parser.add_argument("--driver-header", action="append", type=Path, default=[],
                        help="snapshot a focused driver's additional header (repeatable)")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arm64", action="store_true", help="execute original AArch64 guests with actual Dynarmic")
    parser.add_argument("--dynarmic-source", type=Path, help="pinned, Gesher-patched Dynarmic source")
    parser.add_argument("--dynarmic-build", type=Path, help="matching Linux static Dynarmic build")
    parser.add_argument("--shim-source-ref", help="historical shim wrappers; current complete common loader")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("this explicit host boundary requires Linux x86-64")
    if args.arm64 and not (args.fixtures and args.dynarmic_source and args.dynarmic_build):
        parser.error("--arm64 requires --fixtures, --dynarmic-source and --dynarmic-build")
    cxx, cc = shlex.split(args.cxx), shlex.split(args.cc)
    if args.shim_source_ref:
        reference = subprocess.check_output(["git", "rev-parse", "--verify", "--end-of-options",
            args.shim_source_ref + "^{commit}"], cwd=ROOT, text=True).strip()
        shim = subprocess.check_output(["git", "show", reference + ":compat/bionic/shim.cc"],
                                       cwd=ROOT, text=True)
    else:
        reference = "working-tree snapshot"
        shim = (ROOT / "compat/bionic/shim.cc").read_text()
    start = shim.index("thread_local std::string t_dlerror;")
    implementation = shim[start:shim.index("struct BionicDlInfo", start)]
    entries = []
    for name in ("dlopen", "android_dlopen_ext", "dlsym", "dlvsym", "dlerror", "dlclose"):
        matches = re.findall(r'^[ \t]*ENTRY\("' + name + r'",[^\n]+;', shim, flags=re.MULTILINE)
        if len(matches) != 1:
            raise ValueError("expected one actual libdl registration for " + name)
        entries.append(matches[0])
    print("BOUNDARY complete production ELF loader and unchanged libdl section; Linux Win32 primitives", flush=True)
    print("SHIM_SOURCE_REF " + reference, flush=True)
    print("SHA256 complete_shim " + hashlib.sha256(shim.encode()).hexdigest(), flush=True)
    print("SHA256 actual_libdl " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    # The shared Windows driver also checks the full production provider
    # bootstrap. This route harness deliberately supplies fixture providers;
    # test_bionic_provider_ownership.py covers the actual Registry separately.
    flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
             "-DGESHER_TEST_HOST_LIBDL_BOUNDARY=1"]
    link_flags = ["-pthread"]
    environment = os.environ.copy()
    if args.sanitize:
        sanitizer = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        flags += sanitizer + ["-fno-pie"]
        link_flags += sanitizer + ["-no-pie"]
        previous = environment.get("ASAN_OPTIONS", "")
        environment["ASAN_OPTIONS"] = previous + (":" if previous else "") + "detect_leaks=0"
        print("SANITIZERS address,undefined; fatal findings; LSan disabled under ptrace", flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-guest-libdl-") as temporary:
        folder = Path(temporary)
        elf, fixtures = host.snapshot(folder, None, FIXTURES)
        shutil.copytree(ROOT / "compat/bionic/include", fixtures / "bionic-include")
        if args.driver:
            selected_driver = args.driver.resolve()
            shutil.copy2(selected_driver, fixtures / "guest_libdl_namespace_test.cc")
            print("FOCUSED_DRIVER " + str(selected_driver) + " SHA256 " + host.digest(selected_driver), flush=True)
        driver_headers = fixtures / "driver-include"
        driver_headers.mkdir()
        for header in args.driver_header:
            if (driver_headers / header.name).exists():
                raise ValueError("duplicate focused driver header basename: " + header.name)
            shutil.copy2(header, driver_headers / header.name)
            print("DRIVER_HEADER_SHA256 " + str(header) + " " + host.digest(driver_headers / header.name), flush=True)
        template = (fixtures / "guest_libdl_namespace_host.cc.in").read_text()
        generated = template.replace("@LIBDL_IMPLEMENTATION@", implementation).replace(
            "@LIBDL_ENTRIES@", "\n".join(entries))
        unit = folder / "guest_libdl_boundary.cc"
        unit.write_text(generated)
        print("SHA256 generated_driver " + hashlib.sha256(generated.encode()).hexdigest(), flush=True)
        if args.fixtures:
            graph = args.fixtures.resolve()
        else:
            compiler = folder / "cc-wrapper"
            compiler.write_text("#!/bin/sh\nexec " + shlex.join(cc) + ' "$@"\n')
            compiler.chmod(0o700)
            graph = folder / "graph"
            host.run(["bash", str(fixtures / "build-elf-namespaces.sh"), str(compiler), str(graph)],
                     "BUILD original valid caller/dependency fixtures")
        for path in sorted((graph if args.driver else graph / "caller").rglob("*.so")):
            print("GUEST_SHA256 " + path.relative_to(graph).as_posix() + " " + host.digest(path), flush=True)
        includes = ["-I", str(fixtures / "elf_loader_host_platform"),
                    "-I", str(elf / "include"), "-I", str(elf),
                    "-I", str(fixtures / "bionic-include"),
                    "-I", str(driver_headers),
                    "-I", str(fixtures / "arm64-include")]
        if args.driver:
            includes += ["-iquote", str(selected_driver.parent)]
        arm_sources = [fixtures / "elf_loader_host_arm64.cc"]
        if args.arm64:
            from guest_libdl_arm64_boundary import prepare
            arm_sources, arm_includes, arm_link = prepare(folder, fixtures,
                args.dynarmic_source.resolve(), args.dynarmic_build.resolve())
            includes += arm_includes
            link_flags += arm_link
        contextual = "1" if "void* BIONIC_SYSV LoaderDlopen(" in implementation else "0"
        flags += ["-DGESHER_CONTEXTUAL_LIBDL=" + contextual]
        print("COMPILE_FLAGS " + json.dumps(flags + includes), flush=True)
        sources = [elf / name for name in host.PRODUCTION_UNITS]
        sources += [fixtures / "elf_loader_host_platform.cc", *arm_sources, unit,
                    fixtures / "guest_libdl_namespace_test.cc"]
        objects = []
        for index, path in enumerate(sources):
            target = folder / (str(index) + ".o")
            compile_flags = [] if path.suffix == ".S" else flags
            if path.name == "guest_libdl_arm64.cc":
                # Dynarmic's test callback base contains a constexpr throw in
                # its assertion macro. No fixture or production path throws.
                compile_flags = flags + ["-fexceptions"]
                print("ARM_COMPILE_FLAGS " + json.dumps(compile_flags + includes), flush=True)
            host.run(cxx + compile_flags + includes + ["-c", str(path), "-o", str(target)], "COMPILE " + path.name)
            objects.append(str(target))
        executable = folder / "guest-libdl-namespaces"
        host.run(cxx + objects + link_flags + ["-o", str(executable)], "LINK actual guest libdl runner")
        host.run([str(executable), str(graph)] + (["--arm64"] if args.arm64 else []),
                 "RUN actual ELF caller through production libdl",
                 env=environment)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
