// gesher ELF loader: x86-64 instruction length decoder (x86_decode.h).

#include "x86_decode.h"

namespace gesher::elf {

namespace {

// One-byte opcodes that are invalid in 64-bit mode (and are not prefixes or
// escapes handled separately).
bool Invalid64(uint8_t op) {
  switch (op) {
    case 0x06: case 0x07: case 0x0E: case 0x16: case 0x17: case 0x1E: case 0x1F:
    case 0x27: case 0x2F: case 0x37: case 0x3F: case 0x60: case 0x61: case 0x82:
    case 0x9A: case 0xCE: case 0xD4: case 0xD5: case 0xD6: case 0xEA:
      return true;
    default:
      return false;
  }
}

bool OneByteHasModrm(uint8_t op) {
  if (op < 0x40) return (op & 7) < 4;
  switch (op) {
    case 0x63: case 0x69: case 0x6B: case 0xC0: case 0xC1: case 0xC6: case 0xC7:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xF6: case 0xF7: case 0xFE: case 0xFF:
      return true;
    default:
      break;
  }
  if (op >= 0x80 && op <= 0x8F) return true;
  if (op >= 0xD8 && op <= 0xDF) return true;
  return false;
}

// Immediate size of a one-byte opcode; `reg` is ModRM.reg (for F6/F7).
int OneByteImm(uint8_t op, const X86Insn& in, int reg) {
  int immz = in.opsize ? 2 : 4;
  if (op < 0x40) {
    if ((op & 7) == 4) return 1;
    if ((op & 7) == 5) return immz;
    return 0;
  }
  if (op >= 0x70 && op <= 0x7F) return 1;
  if (op >= 0xB0 && op <= 0xB7) return 1;
  if (op >= 0xB8 && op <= 0xBF) return (in.rex & 8) ? 8 : immz;
  if (op >= 0xA0 && op <= 0xA3) return in.addrsize ? 4 : 8;  // moffs
  if (op >= 0xE0 && op <= 0xE7) return 1;
  switch (op) {
    case 0x68: case 0x69: case 0x81: case 0xA9: case 0xC7: return immz;
    case 0x6A: case 0x6B: case 0x80: case 0x83: case 0xA8: case 0xC0: case 0xC1:
    case 0xC6: case 0xCD: case 0xEB:
      return 1;
    case 0xC2: case 0xCA: return 2;
    case 0xC8: return 3;
    case 0xE8: case 0xE9: return 4;
    case 0xF6: return (reg & 7) < 2 ? 1 : 0;
    case 0xF7: return (reg & 7) < 2 ? immz : 0;
    default: return 0;
  }
}

bool Map1HasModrm(uint8_t op) {
  switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
    case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
      return false;
    default:
      break;
  }
  if (op >= 0x30 && op <= 0x37) return false;
  if (op >= 0x80 && op <= 0x8F) return false;
  if (op >= 0xC8 && op <= 0xCF) return false;
  return true;
}

int Map1Imm(uint8_t op) {
  if (op >= 0x70 && op <= 0x73) return 1;
  if (op >= 0x80 && op <= 0x8F) return 4;
  switch (op) {
    case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4: case 0xC5: case 0xC6:
      return 1;
    default:
      return 0;
  }
}

// Decodes ModRM (+SIB, displacement) at in->modrm_off. Returns the offset just
// past them, or 0 if out of bytes.
size_t DecodeModrm(const uint8_t* p, size_t avail, X86Insn* in) {
  size_t i = in->modrm_off;
  if (i >= avail) return 0;
  in->has_modrm = true;
  in->modrm = p[i++];
  int mod = in->modrm >> 6, rm = in->modrm & 7;
  if (mod == 3) return i;
  if (rm == 4) {
    if (i >= avail) return 0;
    in->has_sib = true;
    in->sib = p[i++];
    if (mod == 0 && (in->sib & 7) == 5) in->disp_size = 4;
  } else if (mod == 0 && rm == 5) {
    in->disp_size = 4;  // RIP-relative
  }
  if (mod == 1) in->disp_size = 1;
  if (mod == 2) in->disp_size = 4;
  in->disp_off = i;
  return i + in->disp_size;
}

}  // namespace

int X86Insn::base() const {
  if (!has_mem()) return -1;
  if (has_sib) {
    int b = sib & 7;
    if (mod() == 0 && b == 5) return -1;
    return b | ((rex & 1) ? 8 : 0);
  }
  if (mod() == 0 && (modrm & 7) == 5) return -1;  // RIP-relative
  return (modrm & 7) | ((rex & 1) ? 8 : 0);
}

int X86Insn::index() const {
  if (!has_mem() || !has_sib) return -1;
  int x = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
  return x == 4 ? -1 : x;
}

bool DecodeX86(const uint8_t* p, size_t avail, X86Insn* out) {
  X86Insn in;
  size_t i = 0;
  // Legacy prefixes, then REX (a REX followed by another prefix is ignored).
  for (;; ++i) {
    if (i >= avail || i >= 15) return false;
    uint8_t b = p[i];
    if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x36 || b == 0x3E ||
        b == 0x26) {
      in.rex = 0;
      continue;
    }
    if (b == 0x64) { in.fs = true; in.rex = 0; continue; }
    if (b == 0x65) { in.gs = true; in.rex = 0; continue; }
    if (b == 0x66) { in.opsize = true; in.rex = 0; continue; }
    if (b == 0x67) { in.addrsize = true; in.rex = 0; continue; }
    if (b >= 0x40 && b <= 0x4F) { in.rex = b; continue; }
    break;
  }
  in.opcode_off = i;
  uint8_t op = p[i];

  // VEX / EVEX / XOP.
  if (op == 0xC5 || op == 0xC4 || op == 0x62 ||
      (op == 0x8F && i + 1 < avail && (p[i + 1] & 0x1F) >= 8)) {
    if (in.rex) return false;  // REX before VEX is #UD
    in.vex = true;
    size_t payload = op == 0xC5 ? 1 : op == 0x62 ? 3 : 2;
    if (i + 1 + payload + 1 > avail) return false;
    uint8_t b1 = p[i + 1];
    uint8_t r = 0;
    if (op == 0xC5) {
      in.map = 1;
      if (!(b1 & 0x80)) r |= 4;
    } else {
      in.map = op == 0x62 ? (b1 & 7) : (b1 & 0x1F);
      if (!(b1 & 0x80)) r |= 4;
      if (!(b1 & 0x40)) r |= 2;
      if (!(b1 & 0x20)) r |= 1;
      if (p[i + 2] & 0x80) r |= 8;
    }
    in.rex = r ? static_cast<uint8_t>(0x40 | r) : 0;
    i += 1 + payload;
    in.opcode = p[i++];
    in.modrm_off = i;
    int imm = 0;
    bool modrm = true;
    if (op == 0x8F) {  // XOP
      imm = in.map == 8 ? 1 : in.map == 0xA ? 4 : 0;
    } else if (in.map == 3) {
      imm = 1;
    } else if (in.map == 1) {
      if (in.opcode == 0x77 && op != 0x62) modrm = false;  // vzeroupper/vzeroall
      int m1 = Map1Imm(in.opcode);
      imm = (in.opcode >= 0x80 && in.opcode <= 0x8F) ? 0 : m1;
    }
    size_t end = i;
    if (modrm) {
      end = DecodeModrm(p, avail, &in);
      if (end == 0) return false;
    }
    in.imm_size = imm;
    in.length = end + imm;
    if (in.length > avail || in.length > 15) return false;
    *out = in;
    return true;
  }

  ++i;
  if (op == 0x0F) {
    if (i >= avail) return false;
    uint8_t op2 = p[i++];
    in.modrm_off = i;
    if (op2 == 0x38 || op2 == 0x3A) {
      if (i >= avail) return false;
      in.map = op2 == 0x38 ? 2 : 3;
      in.opcode = p[i++];
      in.modrm_off = i;
      size_t end = DecodeModrm(p, avail, &in);
      if (end == 0) return false;
      in.imm_size = in.map == 3 ? 1 : 0;
      in.length = end + in.imm_size;
    } else if (op2 == 0x0F) {  // 3DNow!: ModRM, then an imm8 opcode suffix
      in.map = 1;
      in.opcode = op2;
      size_t end = DecodeModrm(p, avail, &in);
      if (end == 0) return false;
      in.imm_size = 1;
      in.length = end + 1;
    } else {
      in.map = 1;
      in.opcode = op2;
      size_t end = i;
      if (Map1HasModrm(op2)) {
        end = DecodeModrm(p, avail, &in);
        if (end == 0) return false;
      }
      int imm = Map1Imm(op2);
      if (op2 == 0x78 && (in.opsize || (p[0] == 0xF2))) imm = 2;  // extrq/insertq
      in.imm_size = imm;
      in.length = end + imm;
    }
  } else {
    if (Invalid64(op)) return false;
    in.map = 0;
    in.opcode = op;
    in.modrm_off = i;
    size_t end = i;
    int reg = 0;
    if (OneByteHasModrm(op)) {
      end = DecodeModrm(p, avail, &in);
      if (end == 0) return false;
      reg = (in.modrm >> 3) & 7;
    }
    in.imm_size = OneByteImm(op, in, reg);
    in.length = end + in.imm_size;
  }
  if (in.length > avail || in.length > 15) return false;
  *out = in;
  return true;
}

}  // namespace gesher::elf
