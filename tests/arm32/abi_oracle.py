#!/usr/bin/env python3
"""Independent scalar AAPCS32 oracle; not a JNI or Android app test.

Compiles actual ARM and Thumb functions, runs them with Unicorn, and supplies
their registers/stack using the production marshaller. The expected raw-value
fingerprints do not use the marshaller's layout algorithm. The compiler's actual
function-pointer state bits are checked: Zig can ignore -mthumb when the target
architecture is arm, so the Thumb build explicitly selects thumb.

Optional host QA (Linux, GCC or Clang):
  python3 -m pip install ziglang==0.17.1 unicorn==2.1.4 pyelftools==0.33
  python3 tests/arm32/abi_oracle.py --cxx g++

Pass --zig /path/to/zig instead of installing ziglang, and --work-dir to retain
the generated C, ELF fixtures and host bridge. No dependencies are downloaded by
this script. The generated fixture is freestanding EABI5 softfp, not an Android
NDK application. No code from another project's tests is used.
"""

from __future__ import annotations

import argparse
import ctypes as ct
import importlib.metadata
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_MODE_ARM, Uc
from unicorn.arm_const import (
    UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
)


MASK64 = (1 << 64) - 1
TYPE_NAMES = {
    1: "int", 2: "unsigned int", 3: "long long", 4: "unsigned long long",
    5: "float", 6: "double", 7: "void*",
}


def require(condition: bool, message: str) -> None:
    # This is an executable acceptance test even when Python is run with -O.
    if not condition:
        raise RuntimeError(message)


def command(arguments: list[str], timeout: int = 120) -> str:
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"command failed: {arguments!r}\n{result.stdout}{result.stderr}")
    return result.stdout.strip()


def make_cases() -> tuple[list[list[int]], list[list[int]], str]:
    rng = random.Random(0xA32A64)
    kinds = list(TYPE_NAMES)
    cases = [[], [1, 6, 1, 3], [1, 1, 1, 3, 1], [5, 5, 6, 7], [1, 1, 1, 1, 1, 6]]
    cases.extend([rng.choice(kinds) for _ in range(rng.randrange(1, 18))] for _ in range(95))
    values = []
    source = ["typedef unsigned int u32; typedef unsigned long long u64;"]
    for index, signature in enumerate(cases):
        values.append([rng.getrandbits(64 if kind in (3, 4, 6) else 32) for kind in signature])
        parameters = ",".join(f"{TYPE_NAMES[kind]} a{j}" for j, kind in enumerate(signature))
        source.append(f"u64 f{index}({parameters or 'void'}) {{ u64 v=0x123456789abcdef0ULL;")
        for j, kind in enumerate(signature):
            if kind == 5:
                source.append(f"union{{float f;u32 u;}} b{j}={{.f=a{j}}};u64 v{j}=b{j}.u;")
            elif kind == 6:
                source.append(f"union{{double d;u64 u;}} b{j}={{.d=a{j}}};u64 v{j}=b{j}.u;")
            else:
                cast = "u64" if kind in (3, 4) else "u32"
                source.append(f"u64 v{j}=({cast})a{j};")
            shift = (j * 7 + 3) % 63 + 1
            source.append(f"v ^= (v{j} << {shift}) | (v{j} >> {64-shift});")
        source.append("return v;}")
    source.extend(["float ret_f(float a){return a;}", "double ret_d(double a){return a;}"])
    return cases, values, "\n".join(source)


def build_bridge(repo: Path, output: Path, compiler: str):
    library = output / "libabi_oracle_bridge.so"
    command([compiler, "-std=c++20", "-O2", "-fPIC", "-shared", "-pthread",
             "-I" + str(repo / "compat/arm32/include"),
             str(repo / "compat/arm32/arm32_abi.cc"),
             str(repo / "tests/arm32/abi_oracle_bridge.cc"), "-o", str(library)])
    bridge = ct.CDLL(str(library))
    bridge.gesher_qa_pack.argtypes = [
        ct.POINTER(ct.c_int), ct.POINTER(ct.c_uint64), ct.c_size_t,
        ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32), ct.c_size_t,
        ct.POINTER(ct.c_size_t),
    ]
    bridge.gesher_qa_pack.restype = ct.c_int
    bridge.gesher_qa_decode.argtypes = [
        ct.c_int, ct.c_uint32, ct.c_uint32, ct.POINTER(ct.c_uint64),
    ]
    bridge.gesher_qa_decode.restype = ct.c_int
    return bridge


def execute(bridge, segments, symbols, name: str, signature: list[int], values: list[int],
            result_kind: int) -> int:
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    mapped = set()
    for address, size, data in segments:
        for page in range(address & ~4095, (address + size + 4095) & ~4095, 4096):
            if page not in mapped:
                cpu.mem_map(page, 4096)
                mapped.add(page)
        cpu.mem_write(address, data)
    stack_base, stack_end, returned = 0x800000, 0x810000, 0x900000
    cpu.mem_map(stack_base, stack_end - stack_base)
    cpu.mem_map(returned, 4096)
    cpu.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
    cpu.reg_write(UC_ARM_REG_FPEXC, 1 << 30)
    kinds = (ct.c_int * len(signature))(*signature)
    bits = (ct.c_uint64 * len(values))(*values)
    core, stack, words = (ct.c_uint32 * 4)(), (ct.c_uint32 * 128)(), ct.c_size_t()
    require(bool(bridge.gesher_qa_pack(kinds, bits, len(signature), core, stack, 128,
                                      ct.byref(words))), f"cannot pack {name}")
    sp = stack_end - words.value * 4
    require(sp >= stack_base and sp % 8 == 0, f"invalid stack for {name}")
    for i in range(words.value):
        cpu.mem_write(sp + i * 4, struct.pack("<I", stack[i]))
    for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), core):
        cpu.reg_write(reg, value)
    cpu.reg_write(UC_ARM_REG_SP, sp)
    cpu.reg_write(UC_ARM_REG_LR, returned)
    cpu.emu_start(symbols[name], returned, timeout=2_000_000, count=100_000)
    require(cpu.reg_read(UC_ARM_REG_PC) == returned, f"{name} did not return within budget")
    actual = ct.c_uint64()
    require(bool(bridge.gesher_qa_decode(result_kind, cpu.reg_read(UC_ARM_REG_R0),
                                        cpu.reg_read(UC_ARM_REG_R1), ct.byref(actual))),
            f"cannot decode {name}'s return")
    return actual.value


def run(repo: Path, output: Path, compiler: str, zig: list[str]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    cases, values, source = make_cases()
    fixture = output / "oracle.c"
    fixture.write_text(source, encoding="utf-8")
    bridge = build_bridge(repo, output, compiler)
    print("Zig:", command(zig + ["version"]))
    print("Unicorn:", importlib.metadata.version("unicorn"))
    print("pyelftools:", importlib.metadata.version("pyelftools"))
    checks = 0
    for mode in ("arm", "thumb"):
        binary = output / f"oracle_{mode}.elf"
        command(zig + ["cc", "-target", f"{mode}-freestanding-eabi", "-mcpu=cortex_a9",
                       "-mfloat-abi=softfp", "-O2", "-nostdlib", "-fno-stack-protector",
                       "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                       "-Wl,-e,f0", str(fixture), "-o", str(binary)])
        with binary.open("rb") as stream:
            elf = ELFFile(stream)
            require(elf.elfclass == 32 and elf["e_machine"] == "EM_ARM" and elf.little_endian,
                    f"{mode}: compiler did not produce little-endian ARM ELF32")
            require(elf["e_flags"] >> 24 == 5 and not (elf["e_flags"] & 0x400),
                    f"{mode}: compiler did not produce EABI5 softfp")
            segments = [(s["p_vaddr"], s["p_memsz"], s.data()) for s in elf.iter_segments()
                        if s["p_type"] == "PT_LOAD"]
            symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        names = [f"f{i}" for i in range(len(cases))] + ["ret_f", "ret_d"]
        for name in names:
            require(name in symbols and symbols[name] & 1 == (mode == "thumb"),
                    f"{mode}: {name} has incorrect ARM/Thumb function-pointer state")
        for index, (signature, arguments) in enumerate(zip(cases, values)):
            expected = 0x123456789ABCDEF0
            for j, value in enumerate(arguments):
                shift = (j * 7 + 3) % 63 + 1
                expected ^= ((value << shift) | (value >> (64 - shift))) & MASK64
            actual = execute(bridge, segments, symbols, f"f{index}", signature, arguments, 4)
            require(actual == expected,
                    f"{mode} f{index} {signature}: got {actual:#x}, expected {expected:#x}")
            checks += 1
        for kind, name, returns in [
            (5, "ret_f", [0, 0x80000000, 0x3F800000, 0x7FC12345, 0xFFFFFFFF]),
            (6, "ret_d", [0, 0x8000000000000000, 0x3FF0000000000000,
                           0x7FF8123456789ABC, 0xFFFFFFFFFFFFFFFF]),
        ]:
            for expected in returns:
                actual = execute(bridge, segments, symbols, name, [kind], [expected], kind)
                require(actual == expected, f"{mode} {name}: FP payload changed")
                checks += 1
        print(f"{mode}: verified function state; 110 actual compiler-generated calls passed")
    print(f"Independent compiler + Unicorn scalar ABI oracle: {checks} calls passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--zig", help="Zig executable (default: python -m ziglang)")
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    require(sys.platform.startswith("linux"), "this optional host oracle currently supports Linux")
    require(shutil.which(args.cxx) is not None, f"C++ compiler not found: {args.cxx}")
    zig = [args.zig] if args.zig else [sys.executable, "-m", "ziglang"]
    repo = Path(__file__).resolve().parents[2]
    if args.work_dir:
        run(repo, args.work_dir.resolve(), args.cxx, zig)
    else:
        with tempfile.TemporaryDirectory(prefix="gesher-arm32-abi-oracle-") as directory:
            run(repo, Path(directory), args.cxx, zig)


if __name__ == "__main__":
    main()
