#!/usr/bin/env python3
"""Build and verify real ARM and Thumb ELF32 fixtures (not Android APKs).

Requires ziglang==0.17.1 and pyelftools==0.33, installed separately. This script
does not download dependencies. Each output deliberately imports only the
explicit host_adjust test adapter; no Bionic/JNI implementation is implied.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from elftools.elf.elffile import ELFFile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--zig", help="Path to a Zig executable; default uses python -m ziglang")
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("guest_fixture.c")
    compiler = [args.zig] if args.zig else [sys.executable, "-m", "ziglang"]
    for mode in ("arm", "thumb"):
        output = (args.out_dir / f"fixture-{mode}.so").resolve()
        subprocess.run(compiler + [
            "cc", "-target", f"{mode}-linux-androideabi", "-mcpu=cortex_a9",
            "-mfloat-abi=softfp", "-mfpu=neon", "-O2", "-fno-sanitize=all",
            "-shared", "-fPIC", "-nostdlib", "-fno-stack-protector",
            "-Wl,--hash-style=both", "-Wl,-soname,gesher_arm32_fixture.so",
            str(source), "-o", str(output),
        ], check=True, timeout=120)
        with output.open("rb") as stream:
            elf = ELFFile(stream)
            if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_ARM":
                raise RuntimeError(f"{output}: compiler did not produce little-endian ARM ELF32")
            symbols = list(elf.get_section_by_name(".dynsym").iter_symbols())
            functions = [symbol for symbol in symbols if symbol.name.startswith("fixture_")]
            if len(functions) != 6 or any(bool(symbol["st_value"] & 1) != (mode == "thumb")
                                          for symbol in functions):
                raise RuntimeError(f"{output}: compiler did not emit the requested {mode} state")
            imports = {symbol.name for symbol in symbols
                       if symbol.name and symbol["st_shndx"] == "SHN_UNDEF"}
            if imports != {"host_adjust"}:
                raise RuntimeError(f"{output}: unexpected runtime dependencies: {sorted(imports)}")
            tags = {tag.entry.d_tag for tag in elf.get_section_by_name(".dynamic").iter_tags()}
            if not {"DT_INIT_ARRAY", "DT_REL", "DT_JMPREL"}.issubset(tags):
                raise RuntimeError(f"{output}: fixture lost its constructor or relocation coverage")
        print(f"Verified {mode} ELF32 fixture: {output}")

        # The second fixture calls real libc symbols through the guest ELF
        # import table. It is not linked to the host's libc or Android ART.
        bionic_source = source.with_name("guest_bionic_fixture.c")
        bionic_output = (args.out_dir / f"bionic-{mode}.so").resolve()
        subprocess.run(compiler + [
            "cc", "-target", f"{mode}-linux-androideabi", "-mcpu=cortex_a9",
            "-mfloat-abi=softfp", "-mfpu=neon", "-O2", "-fno-sanitize=all",
            "-fno-builtin", "-shared", "-fPIC", "-nostdlib",
            "-fno-stack-protector", "-Wl,--hash-style=both",
            "-Wl,-soname,gesher_arm32_bionic_fixture.so",
            str(bionic_source), "-o", str(bionic_output),
        ], check=True, timeout=120)
        with bionic_output.open("rb") as stream:
            elf = ELFFile(stream)
            if elf.elfclass != 32 or not elf.little_endian or elf["e_machine"] != "EM_ARM":
                raise RuntimeError(f"{bionic_output}: not ARM ELF32")
            symbols = list(elf.get_section_by_name(".dynsym").iter_symbols())
            functions = [symbol for symbol in symbols if symbol.name.startswith("bionic_")]
            expected = {"bionic_heap", "bionic_copy", "bionic_strings", "bionic_invalid"}
            if {symbol.name for symbol in functions} != expected or any(
                    bool(symbol["st_value"] & 1) != (mode == "thumb")
                    for symbol in functions):
                raise RuntimeError(f"{bionic_output}: incorrect guest function state")
            imports = {symbol.name for symbol in symbols
                       if symbol.name and symbol["st_shndx"] == "SHN_UNDEF"}
            required_imports = {"malloc", "calloc", "realloc", "free", "strlen",
                                "strnlen", "memcpy", "memmove", "memset",
                                "memcmp", "strcmp", "strncmp"}
            if imports != required_imports:
                raise RuntimeError(
                    f"{bionic_output}: unexpected guest libc imports {sorted(imports)}")
        print(f"Verified {mode} ARM32 libc fixture: {bionic_output}")
        # A genuine two-DSO DT_NEEDED graph. It exercises the Program loader,
        # cross-module PLT fixups and dependency-first guest constructors.
        dependency = (args.out_dir / f"dependency-{mode}.so").resolve()
        consumer = (args.out_dir / f"consumer-{mode}.so").resolve()
        common_flags = [
            "cc", "-target", f"{mode}-linux-androideabi", "-mcpu=cortex_a9",
            "-mfloat-abi=softfp", "-mfpu=neon", "-O2", "-fno-builtin",
            "-shared", "-fPIC", "-nostdlib", "-fno-stack-protector",
            "-Wl,--hash-style=both",
        ]
        subprocess.run(compiler + common_flags + [
            f"-Wl,-soname,dependency-{mode}.so",
            str(source.with_name("guest_linked_dependency.c")),
            "-o", str(dependency),
        ], check=True, timeout=120)
        subprocess.run(compiler + common_flags + [
            f"-Wl,-soname,consumer-{mode}.so",
            str(source.with_name("guest_linked_consumer.c")),
            "-L", str(args.out_dir.resolve()),
            f"-l:dependency-{mode}.so",
            "-o", str(consumer),
        ], check=True, timeout=120)
        with consumer.open("rb") as stream:
            elf = ELFFile(stream)
            if elf.elfclass != 32 or elf["e_machine"] != "EM_ARM":
                raise RuntimeError(f"{consumer}: not ARM ELF32")
            dynamic = elf.get_section_by_name(".dynamic")
            dependencies = [tag.needed for tag in dynamic.iter_tags()
                            if tag.entry.d_tag == "DT_NEEDED"]
            if dependencies != [f"dependency-{mode}.so"]:
                raise RuntimeError(f"{consumer}: missing real DT_NEEDED: {dependencies}")
            undefined = {symbol.name for symbol in
                         elf.get_section_by_name(".dynsym").iter_symbols()
                         if symbol.name and symbol["st_shndx"] == "SHN_UNDEF"}
            if undefined != {"dependency_increment", "dependency_mark_shutdown"}:
                raise RuntimeError(f"{consumer}: unexpected import {undefined}")
        print(f"Verified {mode} guest dependency graph: {consumer}")



if __name__ == "__main__":
    main()
