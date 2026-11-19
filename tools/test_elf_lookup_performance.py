#!/usr/bin/env python3
"""Verify production ELF lookup order, reentry, bounded stack and allocations.

Compiles actual Lookup, GNU/SysV hashes and symbol-table probes. The descriptor
owns real ELF symbol/hash tables; SymbolAddress adapts IFUNC execution to a
reentrant host callback. A deque/visited-set oracle independently checks order.
Actual Android x86-64/ARM64 graph objects remain separate Windows CI gates.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(text: str, begin: str, end: str) -> str:
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError("ELF lookup performance extraction boundary changed")
    return text[text.index(begin):text.index(end)]


def lookup_section(source: str) -> str:
    begin = ("void* LookupVersion(Library* library, const char* name, const char* version"
             if "void* LookupVersion(" in source else "void* Lookup(Library* library, const char* name) {")
    return section(source, begin, "void* LookupGlobal(")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--compare-ref")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    def read(path: str, ref: str | None = args.source_ref) -> str:
        if ref:
            return subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True)
        return (ROOT / path).read_text(encoding="utf-8")

    source = read("compat/elf/elf_loader.cc")
    lookup = lookup_section(source)
    hashes = source[source.index("uint32_t GnuHash(const char* name) {"):source.index("\n}  // namespace")]
    symbols = section(source, "bool SymbolDefined(const Elf64_Sym* sym) {", "// Resolves an IFUNC:")
    # Observe helper calls without changing their hashing/probing logic.
    hashes = hashes.replace("uint32_t GnuHash(const char* name) {", "uint32_t GnuHash(const char* name) { ++gnu_hash_calls;")
    hashes = hashes.replace("uint32_t SysvHash(const char* name) {", "uint32_t SysvHash(const char* name) { ++sysv_hash_calls;")
    versioned = "const SymbolVersion* version = nullptr" in symbols
    marker = ("const SymbolVersion* version = nullptr) {" if versioned
              else "                                   uint32_t sysv_hash) {")
    if symbols.count(marker) != 1:
        raise ValueError("ELF symbol observation boundary changed")
    # Keep the default argument in the shared declaration only; the function
    # body is unchanged apart from the existing visit observation.
    declaration = marker.replace(" = nullptr", "")
    symbols = symbols.replace(marker, declaration + "\n  if (tracing) visited.push_back(const_cast<Library*>(lib));")
    fixture = (ROOT / "tests/native/elf_lookup_performance_host.cc.in").read_text(encoding="utf-8")
    fixture = fixture.replace("@HASHES@", hashes).replace("@SYMBOLS@", symbols)
    with tempfile.TemporaryDirectory(prefix="gesher-elf-lookup-performance-") as temporary:
        work = Path(temporary)
        (work / "elf_types.h").write_text(read("compat/posix/include/elf.h"), encoding="utf-8")
        (work / "envelope.h").write_text((ROOT / "tests/native/elf_lookup_performance_host.h").read_text(encoding="utf-8"), encoding="utf-8")
        (work / "contract.cc").write_text(fixture, encoding="utf-8")
        (work / "lookup.cc").write_text('#include "envelope.h"\n' + lookup, encoding="utf-8")
        flags = ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
                 f"-DHAS_REFERENCE={int(bool(args.compare_ref))}", f"-DHAS_VERSIONS={int(versioned)}"]
        compiler = shlex.split(args.cxx)
        # Bound normal optimized lookup storage independently of sanitizer
        # red zones, spills and frame-pointer decisions. The instrumented
        # object below still executes every runtime contract under ASan/UBSan.
        subprocess.run(compiler + flags + ["-fstack-usage", "-Wframe-larger-than=1024", "-c", "lookup.cc", "-o", "lookup.o"],
                       cwd=work, check=True, timeout=120)
        usage = (work / "lookup.su").read_text(encoding="utf-8")
        frames = [int(line.split("\t")[-2]) for line in usage.splitlines() if "Lookup" in line]
        if not frames or max(frames) > 1024:
            raise ValueError("ELF lookup exceeds the 1024-byte compiled frame bound: " + usage)
        print(f"ELF lookup source SHA256 {hashlib.sha256(lookup.encode()).hexdigest()} normal_fixture_frame_bytes={max(frames)}", flush=True)
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            subprocess.run(compiler + flags + ["-c", "lookup.cc", "-o", "lookup.o"],
                           cwd=work, check=True, timeout=120)
        inputs = ["contract.cc", "lookup.o"]
        if args.compare_ref:
            old = read("compat/elf/elf_loader.cc", args.compare_ref)
            reference = lookup_section(old)
            reference = reference.replace("LookupVersion(", "ReferenceLookupVersion(")
            reference = reference.replace("void* Lookup(", "void* ReferenceLookup(", 1)
            (work / "reference.cc").write_text('#include "envelope.h"\n' + reference, encoding="utf-8")
            inputs += ["reference.cc"]
        executable = work / ("test.exe" if os.name == "nt" else "test")
        command = compiler + flags + inputs + ["-o", str(executable)]
        if os.name == "nt" and not args.sanitize:
            command += ["-static"]
        subprocess.run(command, cwd=work, check=True, timeout=120)
        runtime = os.environ.copy()
        compiler_path = Path(compiler[0])
        if os.name == "nt" and compiler_path.is_absolute():
            runtime["PATH"] = str(compiler_path.parent) + os.pathsep + runtime.get("PATH", "")
        subprocess.run([str(executable), "measure" if args.measure else "gate"], env=runtime, check=True, timeout=120)


if __name__ == "__main__":
    main()
