#!/usr/bin/env python3
"""Execute production jump bytecode with an independent ARM64 CPU emulator.

Requires unicorn==2.1.4. The signal boundary uses the executing Linux thread's
real pthread signal mask; it is not Gesher's Windows Bionic provider. The same
public-API C corpus is separately run in Android and the real Windows loader.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import pathlib
import signal
import struct
import subprocess
import tempfile

import unicorn
import unicorn.arm64_const as arm

ROOT = pathlib.Path(__file__).resolve().parents[1]
HELPER = 0x180000
INVALID_FLAG = 0x181000
CHECKSUM_MISMATCH = 0x182000
CHECKSUM_WORD = 24
STOP = 0x1FF000
OTHER_STOP = STOP + 0x100
STACK = 0x218000
BUFFER = 0x300100
ENTRIES = {"setjmp": 0x100000, "_setjmp": 0x101000, "sigsetjmp": 0x102000, "longjmp": 0x103000}
MASK_A = {signal.SIGUSR1, signal.SIGRTMAX}
MASK_B = {signal.SIGUSR2}


def source(path: str, ref: str | None) -> str:
    if ref is None:
        return (ROOT / path).read_text()
    return subprocess.check_output(["git", "-C", str(ROOT), "show", f"{ref}:{path}"], text=True, stderr=subprocess.PIPE)


def bytecode(directory: pathlib.Path, ref: str | None, cxx: str) -> dict[str, bytes]:
    header = "compat/arm64/dll/guest_setjmp.h"
    try:
        text = source(header, ref)
    except subprocess.CalledProcessError:
        # Run unchanged assertions against the actual old production arrays.
        old = source("compat/arm64/dll/guest_libc.cc", ref)
        begin = old.index("constexpr uint32_t StpX(")
        end = old.index("// --- the floating-point environment", begin)
        text = "#include <cstdint>\n#include <vector>\n" + old[begin:end]
        text += """
namespace gesher_arm64::jump {
enum class SaveMask { Argument, Always, Never };
std::vector<uint32_t> Save(uint64_t, SaveMask, uint64_t) { return {std::begin(kSetjmp), std::end(kSetjmp)}; }
std::vector<uint32_t> Restore(uint64_t) { return {std::begin(kLongjmp), std::end(kLongjmp)}; }
}
"""
    (directory / "guest_setjmp.h").write_text(text)
    # A historical ref may predate the checksum-mismatch entry.
    restore = ("Restore(0x180000, 0x182000)" if "uint64_t checksum_mismatch" in text
               else "Restore(0x180000)")
    generator = directory / "emit.cc"
    generator.write_text("""
#include <fstream>
#include <string>
#include "guest_setjmp.h"
void save(const char* directory, const char* name, const std::vector<uint32_t>& code) {
  std::ofstream out(std::string(directory) + "/" + name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(uint32_t));
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  using namespace gesher_arm64::jump;
  save(argv[1], "setjmp", Save(0x180000, SaveMask::Always, 0x181000));
  save(argv[1], "_setjmp", Save(0x180000, SaveMask::Never, 0x181000));
  save(argv[1], "sigsetjmp", Save(0x180000, SaveMask::Argument, 0x181000));
  save(argv[1], "longjmp", RESTORE);
}
""".replace("RESTORE", restore))
    executable = directory / "emit"
    subprocess.run([cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(generator), "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(directory)], check=True)
    return {name: (directory / name).read_bytes() for name in ENTRIES}


def current_mask() -> set[int]:
    return set(signal.pthread_sigmask(signal.SIG_BLOCK, set()))


class Machine:
    def __init__(self, code: dict[str, bytes]):
        self.cpu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        self.cpu.mem_map(0x100000, 0x100000)
        self.cpu.mem_map(0x200000, 0x20000)
        self.cpu.mem_map(0x300000, 0x10000)
        for name, data in code.items():
            self.cpu.mem_write(ENTRIES[name], data)
        self.cpu.mem_write(HELPER, struct.pack("<I", 0xD65F03C0))
        self.cpu.hook_add(unicorn.UC_HOOK_CODE, self.on_instruction)
        self.calls = 0
        self.invalid = None
        self.mismatch = False
        self.returned = 0
        self.seed(1)

    def on_instruction(self, cpu, address, _size, _data):
        if address in (STOP, OTHER_STOP):
            self.returned = address
            cpu.emu_stop()
        elif address == CHECKSUM_MISMATCH:
            self.mismatch = True
            cpu.emu_stop()
        elif address == INVALID_FLAG:
            self.invalid = cpu.reg_read(arm.UC_ARM64_REG_X0)
            cpu.emu_stop()
        elif address == HELPER:
            assert cpu.reg_read(arm.UC_ARM64_REG_SP) % 16 == 0, "AAPCS64 host-call stack alignment"
            how = cpu.reg_read(arm.UC_ARM64_REG_X0)
            new = cpu.reg_read(arm.UC_ARM64_REG_X1)
            old = cpu.reg_read(arm.UC_ARM64_REG_X2)
            before = current_mask()
            if new:
                assert how == 2, "restoration must use Android SIG_SETMASK"
                bits, = struct.unpack("<Q", cpu.mem_read(new, 8))
                signal.pthread_sigmask(signal.SIG_SETMASK, {n for n in range(1, 65) if bits & (1 << (n - 1))})
            if old:
                cpu.mem_write(old, struct.pack("<Q", sum(1 << (int(n) - 1) for n in before)))
            self.calls += 1
            target = cpu.reg_read(arm.UC_ARM64_REG_LR)
            # Exercise the complete permitted caller-saved clobber set, not
            # just the smaller set currently written by the translator thunk.
            for reg in list(range(0, 18)):
                cpu.reg_write(getattr(arm, f"UC_ARM64_REG_X{reg}"), 0xBAD00000 + reg)
            for reg in range(8):
                cpu.reg_write(getattr(arm, f"UC_ARM64_REG_Q{reg}"), 0xBAD0 + reg)
            cpu.reg_write(arm.UC_ARM64_REG_NZCV, 0xF0000000)
            cpu.reg_write(arm.UC_ARM64_REG_X0, 0)
            cpu.reg_write(arm.UC_ARM64_REG_PC, target)

    def seed(self, seed: int, stack: int = STACK):
        for reg in range(18, 30):
            self.cpu.reg_write(getattr(arm, f"UC_ARM64_REG_X{reg}"), ((seed << 48) | (reg * 0x101010101)) ^ (seed * 13))
        for reg in range(8, 16):
            self.cpu.reg_write(getattr(arm, f"UC_ARM64_REG_Q{reg}"), (0xABCDE << 64) | (seed << 48) | reg)
        self.cpu.reg_write(arm.UC_ARM64_REG_SP, stack)

    def snapshot(self):
        return ([self.cpu.reg_read(getattr(arm, f"UC_ARM64_REG_X{reg}")) for reg in range(18, 30)],
                [self.cpu.reg_read(getattr(arm, f"UC_ARM64_REG_Q{reg}")) & ((1 << 64) - 1) for reg in range(8, 16)],
                self.cpu.reg_read(arm.UC_ARM64_REG_SP))

    def run(self, name: str, buffer: int, value: int, return_address: int = STOP):
        self.cpu.reg_write(arm.UC_ARM64_REG_X0, buffer)
        self.cpu.reg_write(arm.UC_ARM64_REG_X1, value & ((1 << 64) - 1))
        self.cpu.reg_write(arm.UC_ARM64_REG_LR, return_address)
        self.returned = 0
        self.cpu.emu_start(ENTRIES[name], 0, count=10000)
        assert self.returned != 0 or self.invalid is not None or self.mismatch, (
            f"{name} did not return within the bounded instruction budget")
        return self.cpu.reg_read(arm.UC_ARM64_REG_X0) & 0xFFFFFFFF


def one_case(code, name, flag, value):
    machine = Machine(code)
    before = machine.snapshot()
    machine.cpu.mem_write(BUFFER - 16, b"\xA7" * (16 + 256 + 16))
    signal.pthread_sigmask(signal.SIG_SETMASK, MASK_A)
    assert machine.run(name, BUFFER, flag) == 0
    saved = name == "setjmp" or (name == "sigsetjmp" and (flag & 0xFFFFFFFF) != 0)
    signal.pthread_sigmask(signal.SIG_SETMASK, MASK_B)
    machine.seed(2, STACK - 512)
    machine.cpu.reg_write(arm.UC_ARM64_REG_FPCR, 0x400000)
    machine.cpu.reg_write(arm.UC_ARM64_REG_FPSR, 0x10)
    platform = machine.cpu.reg_read(arm.UC_ARM64_REG_X18)
    result = machine.run("longjmp", BUFFER, value, OTHER_STOP)
    expected = value & 0xFFFFFFFF
    assert result == (expected or 1), "longjmp returns the int argument, mapping zero to one"
    assert current_mask() == (MASK_A if saved else MASK_B), f"{name} flag={flag}: wrong restored thread mask"
    assert machine.calls == (2 if saved else 0), "mask-free jumps must not query or change signal state"
    before[0][0] = (platform & ~0x3FFF) | (before[0][0] & 0x3FFF)
    assert machine.snapshot() == before, "saved GPRs/D registers/SP and SCS offset with current base"
    assert machine.returned == STOP, "longjmp must return to the saved PC"
    assert machine.cpu.reg_read(arm.UC_ARM64_REG_FPCR) == 0x400000, "Android does not restore FPCR"
    assert machine.cpu.reg_read(arm.UC_ARM64_REG_FPSR) == 0x10, "Android does not restore FPSR"
    assert machine.cpu.mem_read(BUFFER - 16, 16) == b"\xA7" * 16
    assert machine.cpu.mem_read(BUFFER + 256, 16) == b"\xA7" * 16, "jmp_buf 256-byte ABI bound"


def nested_and_reused(code):
    m = Machine(code)
    signal.pthread_sigmask(signal.SIG_SETMASK, MASK_A)
    m.run("setjmp", BUFFER, 0)
    outer = m.snapshot()
    signal.pthread_sigmask(signal.SIG_SETMASK, MASK_B)
    m.seed(3, STACK - 1024)
    m.run("sigsetjmp", BUFFER + 512, 1)
    inner = m.snapshot()
    signal.pthread_sigmask(signal.SIG_SETMASK, set())
    m.seed(4, STACK - 2048)
    platform = m.cpu.reg_read(arm.UC_ARM64_REG_X18)
    m.run("longjmp", BUFFER + 512, 7)
    inner[0][0] = (platform & ~0x3FFF) | (inner[0][0] & 0x3FFF)
    assert m.snapshot() == inner and current_mask() == MASK_B
    platform = m.cpu.reg_read(arm.UC_ARM64_REG_X18)
    m.run("longjmp", BUFFER, 8)
    outer[0][0] = (platform & ~0x3FFF) | (outer[0][0] & 0x3FFF)
    assert m.snapshot() == outer and current_mask() == MASK_A
    m.run("_setjmp", BUFFER, 1)
    signal.pthread_sigmask(signal.SIG_SETMASK, MASK_B)
    m.run("longjmp", BUFFER, 9)
    assert current_mask() == MASK_B, "_setjmp reuse must clear a prior setjmp saved-mask flag"


def words(machine, buffer):
    return list(struct.unpack("<32Q", machine.cpu.mem_read(buffer, 256)))


def checksum_contract(code):
    # Android 16 stores the XOR of words 0..23 in word 24 after the flag and
    # mask, and siglongjmp verifies it before touching the mask or registers.
    for name, flag in (("setjmp", 0), ("_setjmp", 0), ("sigsetjmp", 0), ("sigsetjmp", 1)):
        m = Machine(code)
        m.cpu.mem_write(BUFFER, bytes([0xA7]) * 256)
        signal.pthread_sigmask(signal.SIG_SETMASK, MASK_A)
        m.run(name, BUFFER, flag)
        saved = words(m, BUFFER)
        expected = 0
        for word in saved[:CHECKSUM_WORD]:
            expected ^= word
        assert saved[CHECKSUM_WORD] == expected, f"{name}({flag}) stores Bionic's checksum word"
    for word in (16, 23, 1, CHECKSUM_WORD, 0, 2, 3, 14, 15):
        for name, flag in (("_setjmp", 0), ("sigsetjmp", 1)):
            m = Machine(code)
            m.cpu.mem_write(BUFFER, bytes([0xA7]) * 256)
            signal.pthread_sigmask(signal.SIG_SETMASK, MASK_A)
            m.run(name, BUFFER, flag)
            calls = m.calls
            corrupted = words(m, BUFFER)
            corrupted[word] ^= 1 << 40
            m.cpu.mem_write(BUFFER, struct.pack("<32Q", *corrupted))
            signal.pthread_sigmask(signal.SIG_SETMASK, MASK_B)
            m.seed(5, STACK - 256)
            before = m.snapshot()
            m.run("longjmp", BUFFER, 3, OTHER_STOP)
            assert m.mismatch and m.returned == 0, f"corrupted word {word} after {name} reaches the checksum abort"
            assert m.calls == calls and current_mask() == MASK_B, "checksum abort precedes mask restoration"
            assert m.snapshot() == before, "checksum abort precedes register restoration"


def assembly_caller(code, directory: pathlib.Path, cc: str):
    output = directory / "registers.elf"
    subprocess.run([cc, "-nostdlib", "-static", "-Wl,--no-undefined", "-Wl,-Ttext=0x110000",
                    "-Wl,--defsym,sigsetjmp=0x102000", "-Wl,--defsym,siglongjmp=0x103000",
                    "-Wl,-e,Arm64SetjmpRegisters", str(ROOT / "tests/native/arm64_setjmp_registers.S"),
                    "-o", str(output)], check=True)
    binary = directory / "registers.bin"
    objcopy = pathlib.Path(cc).resolve().parent / "llvm-objcopy"
    subprocess.run([str(objcopy), "-O", "binary", "--only-section=.text", str(output), str(binary)], check=True)
    machine = Machine(code)
    machine.cpu.mem_write(0x110000, binary.read_bytes())
    machine.cpu.reg_write(arm.UC_ARM64_REG_LR, STOP)
    machine.cpu.emu_start(0x110000, 0, count=10000)
    assert machine.returned == STOP and machine.cpu.reg_read(arm.UC_ARM64_REG_X0) == 0, "independent compiled ARM64 register/FP caller"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="run unchanged cases against a historical Git ref")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--arm-cc", help="NDK aarch64-linux-android*-clang, to execute independent assembly caller too")
    args = parser.parse_args()
    original = current_mask()
    try:
        with tempfile.TemporaryDirectory(prefix="gesher-setjmp-") as temporary:
            directory = pathlib.Path(temporary)
            code = bytecode(directory, args.source_ref, args.cxx)
            cases = 0
            for name in ("setjmp", "_setjmp", "sigsetjmp"):
                for flag in ((0, 1) if name == "sigsetjmp" else (0, 1, 2, -1, -(1 << 31), 1 << 32)):
                    for value in (0, 1, 7, -7, -(1 << 31), (1 << 31) - 1, 1 << 32):
                        one_case(code, name, flag, value)
                        cases += 1
            for flag in (2, -1, -(1 << 31), 1 << 32, (1 << 32) | 1):
                machine = Machine(code)
                before_mask = current_mask()
                machine.run("sigsetjmp", BUFFER, flag)
                assert machine.invalid == flag & ((1 << 64) - 1), "Android rejects flags other than 0/1"
                assert machine.calls == 0 and current_mask() == before_mask
            nested_and_reused(code)
            checksum_contract(code)
            def worker(index):
                saved = current_mask()
                try:
                    for turn in range(12):
                        one_case(code, "sigsetjmp", (turn + index) % 2, turn - 6)
                finally:
                    signal.pthread_sigmask(signal.SIG_SETMASK, saved)
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(worker, range(4)))
            if args.arm_cc:
                assembly_caller(code, directory, args.arm_cc)
            print(f"ARM64 setjmp bytecode: {cases + 48} execution cases passed; nested/reuse/thread masks, checksum and invalid-flag routing passed"
                  + ("; independent compiled register/FP caller passed" if args.arm_cc else ""))
            print("Boundary: Unicorn CPU + real Linux thread mask; not Windows/Dynarmic or Android Bionic execution.")
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, original)


if __name__ == "__main__":
    main()
