// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gesher_arm64::jump {

// Bionic's public ARM64 jmp_buf has 32 64-bit words (sigjmp_buf has 33).
// Its representation is private. Keep the mask and AAPCS64 register groups
// in the Android 16 layout, without importing libc's process-private cookie.
// Android saves x18's low 14 shadow-stack offset bits, retaining the current
// shadow-stack base on restore (SCS_SIZE=16KiB in bionic_constants.h).
// Only the low 64 bits of v8-v15 are callee-saved. FPCR/FPSR are not restored
// by Android's setjmp family.
constexpr uint32_t Pair(bool load, bool fp, int a, int b, int base, int offset) {
  return (fp ? 0x6D000000u : 0xA9000000u) | (load ? 0x00400000u : 0u) |
         ((static_cast<uint32_t>(offset / 8) & 0x7Fu) << 15) |
         (static_cast<uint32_t>(b) << 10) | (static_cast<uint32_t>(base) << 5) |
         static_cast<uint32_t>(a);
}

inline void Pointer(std::vector<uint32_t>& code, uint64_t address) {
  // movz/movk x16: the bound host trampoline may be anywhere in host memory.
  code.push_back(0xD2800010u | (static_cast<uint32_t>(address & 0xffffu) << 5));
  for (unsigned shift = 1; shift < 4; ++shift)
    code.push_back(0xF2800010u | (shift << 21) |
                   (static_cast<uint32_t>((address >> (shift * 16)) & 0xffffu) << 5));
}

enum class SaveMask { Argument, Always, Never };

inline std::vector<uint32_t> Save(uint64_t sigprocmask, SaveMask mode, uint64_t invalid_flag) {
  std::vector<uint32_t> code;
  code.reserve(40);
  if (mode != SaveMask::Argument)
    code.push_back(mode == SaveMask::Always ? 0x52800021u : 0x52800001u); // mov w1, #1/0
  if (mode == SaveMask::Argument) {
    // Android 16 __bionic_setjmp_cookie_get rejects every flag except 0/1.
    // Its ARM64 entry passes x1 to that long argument, including high bits.
    code.push_back(0xF100043Fu); // cmp x1, #1
    std::size_t valid = code.size();
    code.push_back(0);          // b.ls valid flag
    code.push_back(0xAA0103E0u); // mov x0, x1
    Pointer(code, invalid_flag);
    code.push_back(0xD61F0200u); // br x16: non-returning Android abort path
    code[valid] = 0x54000009u | (static_cast<uint32_t>(code.size() - valid) << 5);
  }
  code.push_back(0x910003F0u);  // mov x16, sp
  code.push_back(Pair(false, false, 30, 16, 0, 16));
  for (int reg = 28, offset = 32; reg >= 20; reg -= 2, offset += 16)
    code.push_back(Pair(false, false, reg, reg + 1, 0, offset));
  code.push_back(0x92403643u); // and x3, x18, #0x3fff: preserve only the SCS offset
  code.push_back(Pair(false, false, 3, 19, 0, 112));
  for (int reg = 14, offset = 128; reg >= 8; reg -= 2, offset += 16)
    code.push_back(Pair(false, true, reg, reg + 1, 0, offset));
  code.push_back(0x7100003Fu);  // cmp w1, #0: savemask is an int, not a 64-bit value
  code.push_back(0x1A9F07E2u);  // cset w2, ne
  code.push_back(0xF9000002u);  // str x2, [x0]: clear any previously saved-mask flag
  std::size_t skip = code.size();
  code.push_back(0);           // cbz w2, return
  code.push_back(0xA9BF7BE0u); // stp x0, x30, [sp, #-16]!
  code.push_back(0x91002002u); // add x2, x0, #8: saved LP64 sigset_t
  code.push_back(0xD2800001u); // mov x1, #0: query only
  code.push_back(0x52800000u); // mov w0, #0: ignored when new mask is null
  Pointer(code, sigprocmask);
  code.push_back(0xD63F0200u); // blr x16
  code.push_back(0xA8C17BE0u); // ldp x0, x30, [sp], #16
  code[skip] = 0x34000002u | (static_cast<uint32_t>(code.size() - skip) << 5);
  code.push_back(0x52800000u); // mov w0, #0
  code.push_back(0xD65F03C0u); // ret
  return code;
}

inline std::vector<uint32_t> Restore(uint64_t sigprocmask) {
  std::vector<uint32_t> code;
  code.reserve(32);
  code.push_back(0xB9400002u); // ldr w2, [x0]: saved-mask flag
  std::size_t skip = code.size();
  code.push_back(0);           // cbz w2, restore registers
  code.push_back(0xA9BF07E0u); // stp x0, x1, [sp, #-16]!
  code.push_back(0x91002001u); // add x1, x0, #8: saved mask
  code.push_back(0x52800040u); // mov w0, #2: SIG_SETMASK
  code.push_back(0xD2800002u); // mov x2, #0
  Pointer(code, sigprocmask);
  code.push_back(0xD63F0200u); // blr x16; pending signals may run before the jump
  code.push_back(0xA8C107E0u); // ldp x0, x1, [sp], #16: retain requested return value
  code[skip] = 0x34000002u | (static_cast<uint32_t>(code.size() - skip) << 5);
  code.push_back(Pair(true, false, 30, 16, 0, 16));
  for (int reg = 28, offset = 32; reg >= 20; reg -= 2, offset += 16)
    code.push_back(Pair(true, false, reg, reg + 1, 0, offset));
  code.push_back(Pair(true, false, 3, 19, 0, 112));
  code.push_back(0x9100021Fu); // mov sp, x16
  code.push_back(0x9272C652u); // and x18, x18, #~0x3fff: retain the current SCS base
  code.push_back(0xAA120072u); // orr x18, x3, x18: restore saved SCS offset
  for (int reg = 14, offset = 128; reg >= 8; reg -= 2, offset += 16)
    code.push_back(Pair(true, true, reg, reg + 1, 0, offset));
  code.push_back(0x7100003Fu); // cmp w1, #0
  code.push_back(0x1A9F1420u); // csinc w0, w1, wzr, ne: int value, or 1 for zero
  code.push_back(0xD65F03C0u); // ret to the saved caller
  return code;
}

}  // namespace gesher_arm64::jump
