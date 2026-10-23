// gesher ELF loader: a small x86-64 instruction length decoder.
//
// The %fs rewriter (fs_rewrite.cc) walks a loaded object's code one
// instruction at a time and needs, for each instruction, its length and where
// its prefixes, ModRM, SIB and displacement are. It decodes the 64-bit-mode
// encodings compilers emit: legacy prefixes, REX, the one-byte, 0F, 0F38 and
// 0F3A maps, VEX (C4/C5), EVEX (62), XOP (8F) and 3DNow! (0F 0F). It does not
// validate semantics; an encoding it cannot size returns false.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gesher::elf {

struct X86Insn {
  size_t length = 0;      // total bytes
  size_t opcode_off = 0;  // offset of the first opcode byte (after prefixes/REX)
  uint8_t rex = 0;        // 0 if none
  bool fs = false;        // a 0x64 (%fs) segment prefix
  bool gs = false;        // a 0x65 (%gs) segment prefix
  bool opsize = false;    // 0x66
  bool addrsize = false;  // 0x67
  bool vex = false;       // VEX/EVEX/XOP encoded
  int map = 0;            // 0 one-byte, 1 0F, 2 0F38, 3 0F3A (VEX/EVEX map, XOP 8-10)
  uint8_t opcode = 0;     // the opcode byte within `map`
  bool has_modrm = false;
  size_t modrm_off = 0;
  uint8_t modrm = 0;
  bool has_sib = false;
  uint8_t sib = 0;
  size_t disp_off = 0;    // valid when disp_size > 0
  int disp_size = 0;      // 0, 1 or 4
  int imm_size = 0;

  int mod() const { return modrm >> 6; }
  int reg() const { return ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0); }
  // True if the instruction has a memory operand (ModRM with mod != 3).
  bool has_mem() const { return has_modrm && mod() != 3; }
  // RIP-relative memory operand.
  bool rip_relative() const { return has_mem() && mod() == 0 && (modrm & 7) == 5; }
  // An absolute [disp32] operand: SIB with no base and no index.
  bool absolute() const {
    return has_mem() && has_sib && mod() == 0 && (modrm & 7) == 4 && (sib & 7) == 5 &&
           ((sib >> 3) & 7) == 4 && (rex & 2) == 0;
  }
  // The base register (0-15) of a memory operand, or -1 if none.
  int base() const;
  // The index register (0-15), or -1 if none.
  int index() const;
  int scale() const { return has_sib ? 1 << (sib >> 6) : 1; }
};

// Decodes the instruction at `p` (at most `avail` bytes readable). Returns
// false if the bytes are not an instruction this decoder can size.
bool DecodeX86(const uint8_t* p, size_t avail, X86Insn* out);

}  // namespace gesher::elf
