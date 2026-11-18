#!/usr/bin/env python3
"""Exercise production bionic, EGL and ARM64 symbol registries and resolvers.

The expensive Windows/ART/ANGLE bootstrap is replaced by initially empty tables.
Lookup functions and table types come from production; EGL/ARM64 registration
uses the same owned-key insertion but does not initialize ANGLE/the translator.
--source-ref and --measure reproduce the baseline with the same fixture.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(text: str, begin: str, end: str) -> str:
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError("symbol lookup source boundary changed")
    return text[text.index(begin):text.index(end)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--target", choices=("all", "bionic", "egl", "arm64"), default="all")
    parser.add_argument("--measure", action="store_true", help="report allocations without the zero-allocation gate")
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT, text=True)
        return (ROOT / path).read_text(encoding="utf-8")

    template = (ROOT / "tests/native/symbol_lookup_allocations_host.cc.in").read_text(encoding="utf-8")
    template = template.replace("@HASH_HEADER@", str(ROOT / "compat/arm64/include/gesher/symbol_table.h").replace("\\", "/"))

    def implementation(target: str) -> str:
        if target == "bionic":
            table_type = section(read("compat/bionic/shim_internal.h"), "// Symbol name ->", "// Each part of the shim")
            source = read("compat/bionic/shim.cc")
            lookup = section(source, "void* LookupBionicSymbol(const char* name) {", "void InstallBionicShim() {")
            if "struct BionicRegistry {" in source:
                registry = section(source, "struct BionicRegistry {", "BionicRegistry& Registry() {")
                bootstrap = ("BionicRegistry& Registry() { static BionicRegistry value; return value; }\n"
                             "SymbolTable& Table() { return Registry().aggregate; }\n")
                return table_type + registry + bootstrap + lookup
            return table_type + "SymbolTable& Table() { static SymbolTable table; return table; }\n" + lookup
        if target == "egl":
            source = read("compat/bionic/egl_gles.cc")
            begin = source.index(" ByName() {")
            begin = source.rfind("\n", 0, begin) + 1
            end = source.index("void Register() {", begin)
            return ("#define BIONIC_SYSV\nusing __eglMustCastToProperFunctionPointerType = void(*)();\n" +
                    source[begin:end] + "\nauto& Table() { return ByName(); }\n" +
                    "void* LookupBionicSymbol(const char* name) { return reinterpret_cast<void*>(EglGetProcAddress(name)); }\n" +
                    "void AddBionicSymbol(const char* name, void* value) { ByName()[name] = value; }\n")
        source = read("compat/arm64/dll/guest_libc.cc")
        declaration = source.index(" g_overrides;")
        begin = source.rfind("\n", 0, declaration) + 1
        end = source.index("\n", declaration)
        lookup = section(source, "void* GuestSymbol(const char* name) {", "int64_t GuestRawSyscall(")
        return (source[begin:end] + "\n" + lookup + "\nauto& Table() { return g_overrides; }\n" +
                "void* LookupBionicSymbol(const char* name) { return GuestSymbol(name); }\n" +
                "void AddBionicSymbol(const char* name, void* value) { g_overrides[name] = value; }\n")

    with tempfile.TemporaryDirectory(prefix="gesher-symbol-lookup-") as temporary:
        folder = Path(temporary)
        targets = ("bionic", "egl", "arm64") if args.target == "all" else (args.target,)
        for target in targets:
            source = template.replace("@IMPLEMENTATION@", implementation(target)).replace("@TARGET@", target)
            cpp = folder / f"{target}.cc"
            executable = folder / (target + ".exe" if os.name == "nt" else target)
            cpp.write_text(source, encoding="utf-8")
            command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                                             "-fno-exceptions", "-pthread", str(cpp), "-o", str(executable)]
            if os.name == "nt":
                command += ["-static"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(executable), "measure" if args.measure else "gate"], check=True, timeout=30)


if __name__ == "__main__":
    main()
