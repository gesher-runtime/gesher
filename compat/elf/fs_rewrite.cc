// gesher ELF loader: load-time rewriting of %fs accesses (fs_rewrite.h).

#include "fs_rewrite.h"

#include <elf.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tls.h"
#include "x86_decode.h"

namespace gesher::elf {

namespace {

// NT_TIB.Self: the TEB's own address, per thread and constant for its life.
constexpr int32_t kTebSelf = 0x30;
// bionic's stack-guard slot: %fs:0x28 (TLS_SLOT_STACK_GUARD * 8).
constexpr int32_t kBionicStackGuard = 0x28;

// --- sites the vectored handler knows -----------------------------------------

struct TrapSite {
  uintptr_t addr;        // the int3
  uintptr_t stub;        // where to continue, or 0 for an unsupported access
  const char* reason;    // for an unsupported access (static string)
  const char* lib;       // object name (lives as long as the process)
  uintptr_t lib_bias;    // for printing lib+offset
  uint8_t bytes[15];     // the original instruction
  uint8_t length;
};

struct CodeRange {
  uintptr_t start, end;
  const char* lib;
  uintptr_t lib_bias;
  bool stub_area;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<TrapSite>& Traps() {  // sorted by addr
  static std::vector<TrapSite> v;
  return v;
}
std::vector<CodeRange>& Ranges() {
  static std::vector<CodeRange> v;
  return v;
}
PVOID g_handler = nullptr;
volatile LONG g_reported = 0;

const TrapSite* FindTrapLocked(uintptr_t addr) {
  auto& v = Traps();
  auto it = std::lower_bound(v.begin(), v.end(), addr,
                             [](const TrapSite& s, uintptr_t a) { return s.addr < a; });
  if (it != v.end() && it->addr == addr) return &*it;
  return nullptr;
}

const CodeRange* FindRangeLocked(uintptr_t addr) {
  for (const CodeRange& r : Ranges()) {
    if (addr >= r.start && addr < r.end) return &r;
  }
  return nullptr;
}

void PrintBytes(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) std::fprintf(stderr, " %02x", p[i]);
}

LONG CALLBACK FsVectoredHandler(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_BREAKPOINT) {
    auto addr = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    AcquireSRWLockShared(&g_lock);
    const TrapSite* site = FindTrapLocked(addr);
    TrapSite copy{};
    if (site != nullptr) copy = *site;
    ReleaseSRWLockShared(&g_lock);
    if (site == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (copy.stub != 0) {
      info->ContextRecord->Rip = copy.stub;
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    std::fprintf(stderr,
                 "gesher elf: %s+0x%llx: reached a %%fs access the loader cannot rewrite (%s);"
                 " instruction:",
                 copy.lib, static_cast<unsigned long long>(copy.addr - copy.lib_bias),
                 copy.reason);
    PrintBytes(copy.bytes, copy.length);
    std::fprintf(stderr, "\n  see docs/decisions/0014-native-libs-fs-base.md\n");
    std::fflush(stderr);
    std::abort();
  }
  if (code == EXCEPTION_ACCESS_VIOLATION) {
    // Diagnostics only: name the cause of a fault in loaded code that comes
    // from %fs/%gs thread state, then let the fault take its normal course.
    uintptr_t rip = info->ContextRecord->Rip;
    AcquireSRWLockShared(&g_lock);
    const CodeRange* range = FindRangeLocked(rip);
    CodeRange r{};
    if (range != nullptr) r = *range;
    ReleaseSRWLockShared(&g_lock);
    if (range == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    X86Insn in;
    size_t avail = std::min<uintptr_t>(15, r.end - rip);
    bool decoded = DecodeX86(reinterpret_cast<const uint8_t*>(rip), avail, &in);
    const char* what = nullptr;
    if (decoded && in.fs) {
      what = "an un-rewritten %fs access (Windows keeps no user %fs base)";
    } else if ((r.stub_area || (decoded && in.gs)) && TlsCurrentThreadPointer() == 0) {
      what = "thread-local access on a thread without a TLS block (SetupThreadTls not called)";
    }
    if (what != nullptr && InterlockedIncrement(&g_reported) <= 8) {
      if (r.stub_area) {
        std::fprintf(stderr, "gesher elf: fault in a %%fs stub for %s: %s\n", r.lib, what);
      } else {
        std::fprintf(stderr, "gesher elf: fault at %s+0x%llx: %s\n", r.lib,
                     static_cast<unsigned long long>(rip - r.lib_bias), what);
      }
      std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void EnsureHandler() {
  // Called with g_lock held exclusively.
  if (g_handler == nullptr) g_handler = AddVectoredExceptionHandler(1, FsVectoredHandler);
}

// --- stub memory near the object (for jmp rel32) -----------------------------

constexpr size_t kStubChunk = 0x10000;

struct StubArena {
  uint8_t* base = nullptr;
  size_t used = 0;
};

// Allocates kStubChunk bytes as close as possible to [lo, hi), within +-2 GiB
// of both ends if it can. Falls back to anywhere.
uint8_t* AllocNear(uintptr_t lo, uintptr_t hi) {
  const uintptr_t gran = 0x10000;
  for (int dir = 0; dir < 2; ++dir) {
    for (uintptr_t step = 0; step < 0x40000000; step += gran) {
      uintptr_t want = dir == 0 ? ((hi + gran - 1) & ~(gran - 1)) + step
                                : (lo & ~(gran - 1)) - gran - step;
      if (dir == 1 && want > lo) break;  // wrapped
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(reinterpret_cast<void*>(want), &mbi, sizeof(mbi)) == 0) continue;
      if (mbi.State != MEM_FREE) continue;
      void* p = VirtualAlloc(reinterpret_cast<void*>(want), kStubChunk, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
      if (p != nullptr) return static_cast<uint8_t*>(p);
    }
  }
  return static_cast<uint8_t*>(
      VirtualAlloc(nullptr, kStubChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

bool Rel32Reachable(uintptr_t from_next, uintptr_t to) {
  int64_t d = static_cast<int64_t>(to) - static_cast<int64_t>(from_next);
  return d >= INT32_MIN && d <= INT32_MAX;
}

// --- the rewriting ------------------------------------------------------------

int32_t ReadI32(const uint8_t* p) {
  int32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
void WriteI32(uint8_t* p, int32_t v) { std::memcpy(p, &v, 4); }

// For a register-based %fs operand: decides whether a stub can replay the
// instruction. Sets *adj (the register to add the thread pointer to) and
// *restore (whether to subtract it again afterwards). Returns nullptr if it
// can, else the reason.
const char* PlanStub(const X86Insn& in, int* adj, bool* restore, bool* writes_sp = nullptr) {
  if (in.vex) return "a VEX/EVEX instruction with a register-based %fs operand";
  if (in.addrsize) return "a 32-bit address-size %fs operand";
  int base = in.base(), index = in.index();
  if (base == 4) return "an %rsp-based %fs operand";
  if (base >= 0) {
    if (base == index) return "the same register as %fs base and index";
    *adj = base;
  } else if (index >= 0 && in.scale() == 1) {
    *adj = index;
  } else {
    return "a %fs operand with no usable base register";
  }

  // What the instruction does with its ModRM.reg operand.
  enum { kNoRegWrite, kRegWrite, kRegCondWrite } kind;
  bool byte_op = false;
  int size;  // operand size of the register written, in bits
  const uint8_t op = in.opcode;
  const int reg_field = (in.modrm >> 3) & 7;
  if (in.map == 0) {
    switch (op) {
      case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x12: case 0x13: case 0x1A:
      case 0x1B: case 0x22: case 0x23: case 0x2A: case 0x2B: case 0x32: case 0x33:
      case 0x8A: case 0x8B: case 0x86: case 0x87:
        kind = kRegWrite;
        byte_op = (op & 1) == 0;
        break;
      case 0x63:
        kind = kRegWrite;
        break;
      case 0x00: case 0x01: case 0x08: case 0x09: case 0x10: case 0x11: case 0x18:
      case 0x19: case 0x20: case 0x21: case 0x28: case 0x29: case 0x30: case 0x31:
      case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x84: case 0x85: case 0x88:
      case 0x89: case 0x80: case 0x81: case 0x83:
        kind = kNoRegWrite;
        break;
      case 0xC6: case 0xC7:
        if (reg_field != 0) return "an unsupported opcode";
        kind = kNoRegWrite;
        break;
      case 0xF6: case 0xF7:
        if (reg_field != 0 && reg_field != 2 && reg_field != 3) {
          return "mul/div with a %fs operand";
        }
        kind = kNoRegWrite;
        break;
      case 0xFE: case 0xFF:
        if (reg_field > 1) return "an unsupported opcode";
        kind = kNoRegWrite;
        break;
      default:
        return "an unsupported opcode";
    }
  } else if (in.map == 1) {
    if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF || op == 0xAF) {
      kind = kRegWrite;
    } else if (op >= 0x40 && op <= 0x4F) {
      kind = kRegCondWrite;
    } else {
      return "an unsupported opcode";
    }
  } else {
    return "an unsupported opcode";
  }
  size = byte_op ? 8 : (in.rex & 8) ? 64 : in.opsize ? 16 : 32;
  if (writes_sp) {
    bool reg_operand = kind != kNoRegWrite ||
        (in.map == 0 && (op <= 0x3B || op == 0x84 || op == 0x85 || op == 0x88 || op == 0x89));
    bool byte_register = byte_op || (kind == kNoRegWrite && reg_operand && (op & 1) == 0);
    int operand = in.reg();
    if (byte_register && !in.rex && operand >= 4 && operand < 8) operand -= 4;
    *writes_sp = reg_operand && operand == 4;
  }

  *restore = true;
  if (kind != kNoRegWrite) {
    int dest = in.reg();
    // Without REX, byte registers 4-7 are %ah..%bh, the second byte of 0-3.
    int aliased = (byte_op && !in.rex && dest >= 4 && dest < 8) ? dest - 4 : dest;
    if (aliased == *adj) {
      if (kind == kRegCondWrite) return "a cmov into its own %fs base register";
      if (size < 32) return "a partial write of its own %fs base register";
      *restore = false;  // the result overwrites the adjusted register
    }
  }
  return nullptr;
}

const char* PlanAbsoluteStub(const X86Insn& in, int* scratch) {
  if (in.vex) return "a VEX/EVEX local-exec TLS instruction";
  if (in.addrsize) return "a 32-bit address-size local-exec TLS instruction";
  // SIMD and x87 memory operands do not use a GPR for their value. Keep their
  // original instruction bytes and preserve the scratch GPR around them.
  if ((in.map == 0 && in.opcode >= 0xD8 && in.opcode <= 0xDF) ||
      (in.map == 1 && (in.opcode == 0x10 || in.opcode == 0x11 || in.opcode == 0x28 ||
                      in.opcode == 0x29 || in.opcode == 0x2E || in.opcode == 0x2F ||
                      in.opcode == 0x6E || in.opcode == 0x6F || in.opcode == 0x7E ||
                      in.opcode == 0x7F || (in.opcode >= 0x54 && in.opcode <= 0x5F)))) {
    *scratch = 0;
    return nullptr;
  }
  // Avoid both the ModRM register and its legacy high-byte alias.
  *scratch = (in.reg() & 3) == 0 ? 1 : 0;
  if (in.map == 1 && (in.opcode == 0x2C || in.opcode == 0x2D))
    return in.reg() == 4 ? "a local-exec TLS conversion using the stack pointer" : nullptr;
  X86Insn replay = in;
  replay.rex &= ~1;
  replay.modrm = static_cast<uint8_t>((in.modrm & 0x38) | 0x84); // mod=2, SIB
  replay.sib = static_cast<uint8_t>(0x20 | *scratch); // no index, scratch base
  int adjusted = -1;
  bool restore = true, writes_sp = false;
  const char* why = PlanStub(replay, &adjusted, &restore, &writes_sp);
  if (why) return why;
  return writes_sp ? "a local-exec TLS instruction using the stack pointer" : nullptr;
}

uint8_t* EmitAbsoluteStub(uint8_t* out, const uint8_t* site, const X86Insn& in,
                          int scratch, int32_t slot) {
  const uint8_t down[] = {0x48, 0x8D, 0x64, 0x24, 0x80};
  const uint8_t up[] = {0x48, 0x8D, 0xA4, 0x24, 0x80, 0, 0, 0};
  std::memcpy(out, down, sizeof(down));
  out += sizeof(down);
  *out++ = static_cast<uint8_t>(0x50 + scratch); // push below the red zone
  *out++ = 0x65;
  *out++ = 0x48;
  *out++ = 0x8B;
  *out++ = static_cast<uint8_t>((scratch << 3) | 4);
  *out++ = 0x25;
  WriteI32(out, slot);
  out += 4; // mov %gs:slot,scratch; no flags changed
  for (size_t i = 0; i < in.length; ++i) {
    if (i < in.opcode_off && site[i] == 0x64) continue;
    uint8_t byte = site[i];
    if (i < in.opcode_off && (byte & 0xF0) == 0x40) byte &= ~1;
    if (i == in.modrm_off) byte = static_cast<uint8_t>((byte & 0x38) | 0x84);
    if (i == in.modrm_off + 1) byte = static_cast<uint8_t>(0x20 | scratch);
    *out++ = byte;
  }
  *out++ = static_cast<uint8_t>(0x58 + scratch);
  std::memcpy(out, up, sizeof(up));
  return out + sizeof(up);
}

// Emits `lea -128(%rsp),%rsp; pushfq; add|sub %gs:slot,%reg; popfq;
// lea 128(%rsp),%rsp`, which moves %reg by the thread pointer without touching
// the flags or the red zone.
uint8_t* EmitAdjust(uint8_t* p, int reg, bool add, int32_t slot) {
  const uint8_t lea_down[] = {0x48, 0x8D, 0x64, 0x24, 0x80};
  const uint8_t lea_up[] = {0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00};
  std::memcpy(p, lea_down, sizeof(lea_down));
  p += sizeof(lea_down);
  *p++ = 0x9C;  // pushfq
  *p++ = 0x65;  // %gs
  *p++ = static_cast<uint8_t>(0x48 | (reg >= 8 ? 4 : 0));
  *p++ = add ? 0x03 : 0x2B;
  *p++ = static_cast<uint8_t>(((reg & 7) << 3) | 4);
  *p++ = 0x25;  // SIB: no base, no index -> [disp32]
  WriteI32(p, slot);
  p += 4;
  *p++ = 0x9D;  // popfq
  std::memcpy(p, lea_up, sizeof(lea_up));
  p += sizeof(lea_up);
  return p;
}
constexpr size_t kAdjustSize = 5 + 1 + 9 + 1 + 8;
constexpr size_t kMaxStub = 2 * kAdjustSize + 15 + 14;

struct Region {
  uint64_t start, end;  // vaddrs
};

// ULEB/SLEB and DWARF EH pointer encodings, for .eh_frame_hdr and FDEs.
uint64_t ReadUleb(const uint8_t** p) {
  uint64_t v = 0;
  int shift = 0;
  uint8_t b;
  do {
    b = *(*p)++;
    if (shift < 64) v |= static_cast<uint64_t>(b & 0x7F) << shift;
    shift += 7;
  } while (b & 0x80);
  return v;
}
int64_t ReadSleb(const uint8_t** p) {
  int64_t v = 0;
  int shift = 0;
  uint8_t b;
  do {
    b = *(*p)++;
    if (shift < 64) v |= static_cast<int64_t>(b & 0x7F) << shift;
    shift += 7;
  } while (b & 0x80);
  if (shift < 64 && (b & 0x40)) v |= -(int64_t{1} << shift);
  return v;
}
// Reads a value in encoding format `enc & 0x0F` (no application). Returns
// false for a format it does not know.
bool ReadEncodedValue(const uint8_t** p, uint8_t enc, uint64_t* out) {
  const uint8_t* q = *p;
  switch (enc & 0x0F) {
    case 0x00: { uint64_t v; std::memcpy(&v, q, 8); *out = v; q += 8; break; }
    case 0x01: *out = ReadUleb(&q); break;
    case 0x02: { uint16_t v; std::memcpy(&v, q, 2); *out = v; q += 2; break; }
    case 0x03: { uint32_t v; std::memcpy(&v, q, 4); *out = v; q += 4; break; }
    case 0x04: { uint64_t v; std::memcpy(&v, q, 8); *out = v; q += 8; break; }
    case 0x09: *out = static_cast<uint64_t>(ReadSleb(&q)); break;
    case 0x0A: { int16_t v; std::memcpy(&v, q, 2); *out = static_cast<uint64_t>(v); q += 2; break; }
    case 0x0B: { int32_t v; std::memcpy(&v, q, 4); *out = static_cast<uint64_t>(v); q += 4; break; }
    case 0x0C: { int64_t v; std::memcpy(&v, q, 8); *out = static_cast<uint64_t>(v); q += 8; break; }
    default: return false;
  }
  *p = q;
  return true;
}

// The FDE pointer encoding ('R') of the CIE at `cie`, or -1.
int CieFdeEncoding(const uint8_t* cie) {
  const uint8_t* p = cie;
  uint32_t len32;
  std::memcpy(&len32, p, 4);
  p += 4;
  if (len32 == 0xFFFFFFFFu) p += 8;
  uint32_t id;
  std::memcpy(&id, p, 4);
  p += 4;
  if (id != 0) return -1;
  uint8_t version = *p++;
  const char* aug = reinterpret_cast<const char*>(p);
  p += std::strlen(aug) + 1;
  ReadUleb(&p);  // code alignment
  ReadSleb(&p);  // data alignment
  if (version == 1) ++p; else ReadUleb(&p);  // return address register
  int enc = 0x00;  // absptr by default
  if (aug[0] != 'z') return enc;
  ReadUleb(&p);  // augmentation data length
  for (const char* a = aug + 1; *a; ++a) {
    if (*a == 'R') {
      enc = *p++;
    } else if (*a == 'P') {
      uint8_t penc = *p++;
      uint64_t dummy;
      if (!ReadEncodedValue(&p, penc, &dummy)) return -1;
    } else if (*a == 'L') {
      ++p;
    } else if (*a == 'S' || *a == 'B' || *a == 'G') {
      // no data
    } else {
      break;
    }
  }
  return enc;
}

// Code regions from .eh_frame_hdr: each FDE's [pc_begin, pc_begin + range).
bool RegionsFromEhFrameHdr(const uint8_t* hdr, uint8_t* load_bias, std::vector<Region>* out) {
  if (hdr[0] != 1) return false;
  uint8_t eh_ptr_enc = hdr[1], count_enc = hdr[2], table_enc = hdr[3];
  const uint8_t* p = hdr + 4;
  uint64_t v;
  if (eh_ptr_enc == 0xFF || !ReadEncodedValue(&p, eh_ptr_enc, &v)) return false;
  if (count_enc == 0xFF || !ReadEncodedValue(&p, count_enc, &v)) return false;
  uint64_t count = v;
  if (table_enc != 0x3B || count == 0) return false;  // datarel | sdata4
  const auto bias = reinterpret_cast<uintptr_t>(load_bias);
  const auto hdr_addr = reinterpret_cast<uintptr_t>(hdr);
  for (uint64_t i = 0; i < count; ++i, p += 8) {
    uintptr_t pc = hdr_addr + static_cast<intptr_t>(ReadI32(p));
    const uint8_t* fde = reinterpret_cast<const uint8_t*>(hdr_addr +
                                                          static_cast<intptr_t>(ReadI32(p + 4)));
    const uint8_t* q = fde;
    uint32_t len32;
    std::memcpy(&len32, q, 4);
    q += 4;
    bool wide = len32 == 0xFFFFFFFFu;
    if (wide) q += 8;
    const uint8_t* cie_field = q;
    uint64_t cie_off;
    if (wide) {
      std::memcpy(&cie_off, q, 8);
      q += 8;
    } else {
      uint32_t c;
      std::memcpy(&c, q, 4);
      cie_off = c;
      q += 4;
    }
    int enc = CieFdeEncoding(cie_field - cie_off);
    if (enc < 0) continue;
    uint64_t begin_raw, range;
    if (!ReadEncodedValue(&q, static_cast<uint8_t>(enc), &begin_raw)) continue;
    if (!ReadEncodedValue(&q, static_cast<uint8_t>(enc & 0x0F), &range)) continue;
    if (range == 0) continue;
    out->push_back({pc - bias, pc - bias + range});
  }
  return !out->empty();
}

}  // namespace

bool RewriteFsAccesses(const char* name, const uint8_t* file, size_t file_size,
                       uint8_t* load_bias, FsRewriteStats* stats, std::string* error,
                       size_t executable_tls_size) {
  *stats = FsRewriteStats{};
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(file + ehdr->e_phoff);
  const auto bias = reinterpret_cast<uintptr_t>(load_bias);

  // Executable file-backed ranges, and .eh_frame_hdr.
  std::vector<Region> exec;
  const uint8_t* eh_frame_hdr = nullptr;
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X) && phdr[i].p_filesz > 0) {
      exec.push_back({phdr[i].p_vaddr, phdr[i].p_vaddr + phdr[i].p_filesz});
    } else if (phdr[i].p_type == PT_GNU_EH_FRAME) {
      eh_frame_hdr = load_bias + phdr[i].p_vaddr;
    }
  }
  if (exec.empty()) return true;

  // Where the code is: FDE ranges, else executable sections, else whole
  // executable segments.
  std::vector<Region> regions;
  if (eh_frame_hdr != nullptr && RegionsFromEhFrameHdr(eh_frame_hdr, load_bias, &regions)) {
    stats->used_fdes = true;
  } else {
    regions.clear();
    if (ehdr->e_shoff != 0 && ehdr->e_shentsize == sizeof(Elf64_Shdr) &&
        ehdr->e_shoff + uint64_t{ehdr->e_shnum} * sizeof(Elf64_Shdr) <= file_size) {
      const auto* shdr = reinterpret_cast<const Elf64_Shdr*>(file + ehdr->e_shoff);
      for (int i = 0; i < ehdr->e_shnum; ++i) {
        if (shdr[i].sh_type == SHT_PROGBITS && (shdr[i].sh_flags & SHF_EXECINSTR) &&
            (shdr[i].sh_flags & SHF_ALLOC) && shdr[i].sh_size > 0) {
          regions.push_back({shdr[i].sh_addr, shdr[i].sh_addr + shdr[i].sh_size});
        }
      }
    }
    if (regions.empty()) regions = exec;
  }
  // Clip to the executable segments, sort and merge.
  std::vector<Region> code;
  for (const Region& r : regions) {
    for (const Region& e : exec) {
      uint64_t s = std::max(r.start, e.start), t = std::min(r.end, e.end);
      if (s < t) code.push_back({s, t});
    }
  }
  std::sort(code.begin(), code.end(),
            [](const Region& a, const Region& b) { return a.start < b.start; });
  std::vector<Region> merged;
  for (const Region& r : code) {
    if (!merged.empty() && r.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, r.end);
    } else {
      merged.push_back(r);
    }
  }

  int32_t slot = 0;  // the TEB offset of the thread-pointer slot, on demand
  auto need_slot = [&]() -> bool {
    if (slot == 0) slot = static_cast<int32_t>(TlsThreadPointerSlot());
    if (slot == 0) {
      if (error) *error = std::string(name) + ": no inline TLS slot for the %fs rewrite";
      return false;
    }
    return true;
  };

  // The object's whole executable span, for stub placement and diagnostics.
  uintptr_t span_lo = UINTPTR_MAX, span_hi = 0;
  for (const Region& e : exec) {
    span_lo = std::min<uintptr_t>(span_lo, bias + e.start);
    span_hi = std::max<uintptr_t>(span_hi, bias + e.end);
  }

  std::vector<StubArena> arenas;
  std::vector<TrapSite> traps;
  // Names must outlive the object; the loader never frees them, but keep our
  // own copy so the handler never reads freed memory.
  char* lib_name = _strdup(name);

  auto alloc_stub = [&](size_t n) -> uint8_t* {
    if (arenas.empty() || arenas.back().used + n > kStubChunk) {
      uint8_t* chunk = AllocNear(span_lo, span_hi);
      if (chunk == nullptr) return nullptr;
      arenas.push_back({chunk, 0});
    }
    uint8_t* p = arenas.back().base + arenas.back().used;
    arenas.back().used += n;
    return p;
  };

  auto add_trap = [&](uint8_t* site, const X86Insn& in, uintptr_t stub, const char* reason) {
    TrapSite t{};
    t.addr = reinterpret_cast<uintptr_t>(site);
    t.stub = stub;
    t.reason = reason;
    t.lib = lib_name;
    t.lib_bias = bias;
    t.length = static_cast<uint8_t>(in.length);
    std::memcpy(t.bytes, site, in.length);
    traps.push_back(t);
    site[0] = 0xCC;  // int3
  };

  auto unsupported = [&](uint8_t* site, const X86Insn& in, const char* reason) {
    std::fprintf(stderr,
                 "gesher elf: %s+0x%llx: %%fs access not rewritten (%s); it aborts if reached\n",
                 name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site) - bias),
                 reason);
    add_trap(site, in, 0, reason);
    ++stats->trapped;
  };

  for (const Region& r : merged) {
    uint8_t* p = load_bias + r.start;
    uint8_t* end = load_bias + r.end;
    while (p < end) {
      X86Insn in;
      if (!DecodeX86(p, static_cast<size_t>(end - p), &in)) {
        ++stats->decode_errors;
        ++p;
        continue;
      }
      stats->bytes_decoded += in.length;
      uint8_t* site = p;
      p += in.length;
      if (!in.fs || !in.has_mem()) continue;  // %fs on a non-memory form is inert
      if (in.map == 0 && in.opcode == 0x8D) continue;  // lea ignores the segment
      if (in.map == 1 && !in.vex && (in.opcode >= 0x18 && in.opcode <= 0x1F)) continue;  // hints/nop
      if (in.map == 1 && !in.vex && in.opcode == 0x0D) continue;  // prefetchw
      if (in.gs) {
        unsupported(site, in, "both %fs and %gs prefixes");
        continue;
      }
      if (in.absolute()) {
        int32_t disp = ReadI32(site + in.disp_off);
        if (disp < 0 && static_cast<uint64_t>(-static_cast<int64_t>(disp)) <= executable_tls_size) {
          int scratch = -1;
          if (const char* why = PlanAbsoluteStub(in, &scratch)) {
            unsupported(site, in, why);
            continue;
          }
          if (!need_slot()) return false;
          uint8_t* stub = alloc_stub(kMaxStub);
          if (!stub) {
            if (error) *error = std::string(name) + ": out of memory for local-exec TLS stubs";
            return false;
          }
          uint8_t* tail = EmitAbsoluteStub(stub, site, in, scratch, slot);
          const uint8_t jump[] = {0xFF, 0x25, 0, 0, 0, 0};
          std::memcpy(tail, jump, sizeof(jump));
          uint64_t back = reinterpret_cast<uint64_t>(site + in.length);
          std::memcpy(tail + sizeof(jump), &back, 8);
          ++stats->stubbed;
          uintptr_t next = reinterpret_cast<uintptr_t>(site) + 5;
          if (in.length >= 5 && Rel32Reachable(next, reinterpret_cast<uintptr_t>(stub))) {
            site[0] = 0xE9;
            WriteI32(site + 1, static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) -
                                                  static_cast<intptr_t>(next)));
            for (size_t i = 5; i < in.length; ++i) site[i] = 0xCC;
          } else {
            add_trap(site, in, reinterpret_cast<uintptr_t>(stub), nullptr);
          }
          continue;
        }
        int32_t to;
        if (disp == kBionicStackGuard) {
          to = kTebSelf;
          ++stats->stack_guard;
        } else if (disp == 0) {
          if (!need_slot()) return false;
          to = slot;
          ++stats->thread_ptr;
        } else {
          unsupported(site, in, "a %fs displacement other than 0 (thread pointer) or 0x28 (guard)");
          continue;
        }
        for (size_t i = 0; i < in.opcode_off; ++i) {
          if (site[i] == 0x64) site[i] = 0x65;
        }
        WriteI32(site + in.disp_off, to);
        continue;
      }
      if (in.rip_relative()) {
        unsupported(site, in, "a RIP-relative %fs operand");
        continue;
      }
      int adj = -1;
      bool restore = true;
      if (const char* why = PlanStub(in, &adj, &restore)) {
        unsupported(site, in, why);
        continue;
      }
      if (!need_slot()) return false;
      uint8_t* stub = alloc_stub(kMaxStub);
      if (stub == nullptr) {
        if (error) *error = std::string(name) + ": out of memory for %fs stubs";
        return false;
      }
      uint8_t* s = EmitAdjust(stub, adj, true, slot);
      for (size_t i = 0; i < in.length; ++i) {  // the instruction without 0x64
        if (i < in.opcode_off && site[i] == 0x64) continue;
        *s++ = site[i];
      }
      if (restore) s = EmitAdjust(s, adj, false, slot);
      const uint8_t jmp_abs[] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp *0(%rip)
      std::memcpy(s, jmp_abs, sizeof(jmp_abs));
      s += sizeof(jmp_abs);
      uint64_t back = reinterpret_cast<uint64_t>(site + in.length);
      std::memcpy(s, &back, 8);
      ++stats->stubbed;
      uintptr_t stub_addr = reinterpret_cast<uintptr_t>(stub);
      uintptr_t next = reinterpret_cast<uintptr_t>(site) + 5;
      if (in.length >= 5 && Rel32Reachable(next, stub_addr)) {
        site[0] = 0xE9;  // jmp rel32
        WriteI32(site + 1, static_cast<int32_t>(static_cast<int64_t>(stub_addr) -
                                                static_cast<int64_t>(next)));
        for (size_t i = 5; i < in.length; ++i) site[i] = 0xCC;
      } else {
        add_trap(site, in, stub_addr, nullptr);
      }
    }
  }

  // Make the stubs executable.
  for (const StubArena& a : arenas) {
    DWORD old;
    if (!VirtualProtect(a.base, kStubChunk, PAGE_EXECUTE_READ, &old)) {
      if (error) *error = std::string(name) + ": cannot make %fs stubs executable";
      return false;
    }
    FlushInstructionCache(GetCurrentProcess(), a.base, kStubChunk);
  }

  AcquireSRWLockExclusive(&g_lock);
  EnsureHandler();
  auto& all = Traps();
  all.insert(all.end(), traps.begin(), traps.end());
  std::sort(all.begin(), all.end(),
            [](const TrapSite& a, const TrapSite& b) { return a.addr < b.addr; });
  for (const Region& e : exec) {
    Ranges().push_back({bias + e.start, bias + e.end, lib_name, bias, false});
  }
  for (const StubArena& a : arenas) {
    Ranges().push_back({reinterpret_cast<uintptr_t>(a.base),
                        reinterpret_cast<uintptr_t>(a.base) + kStubChunk, lib_name, bias, true});
  }
  ReleaseSRWLockExclusive(&g_lock);
  return true;
}

}  // namespace gesher::elf
