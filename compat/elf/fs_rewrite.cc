// gesher ELF loader: load-time rewriting of %fs accesses (fs_rewrite.h).

#include "fs_rewrite.h"

#include <elf.h>

#include <sys/mman.h>
#include <unistd.h>
#include <windows.h>

#include <errno.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "tls.h"
#include "x86_decode.h"

// Internal POSIX mapper outcome, consumed immediately after a failed mmap.
extern "C" int gesher_mmap_replaced_private_pages(void);

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

// Raw system calls and code rewritten on execution (ADR 0084); defined below.
bool HandleSyscallTrap(EXCEPTION_POINTERS* info);
bool HandleExecuteFault(uintptr_t addr);

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

#if defined(__x86_64__)
// Stripped callbacks can be reachable only through indirect pointers, with no
// FDE or function symbol. Recover these two exact read-only MOV forms when
// they actually execute; never scan data tables or modify executable bytes.
bool RecoverUncoveredMov(EXCEPTION_POINTERS* info, const X86Insn& in, const CodeRange& range) {
  if (range.stub_area || !in.fs || in.gs || in.vex || in.map != 0 || in.opcode != 0x8B
      || !(in.rex & 8) || in.opsize || in.addrsize || !in.absolute() || in.disp_size != 4
      || info->ExceptionRecord->NumberParameters < 2
      || info->ExceptionRecord->ExceptionInformation[0] != 0) return false;
  int32_t displacement;
  std::memcpy(&displacement, reinterpret_cast<const uint8_t*>(info->ContextRecord->Rip) + in.disp_off, 4);
  if ((displacement != 0 && displacement != kBionicStackGuard)
      || info->ExceptionRecord->ExceptionInformation[1] != static_cast<uintptr_t>(displacement)) return false;
  uintptr_t value = displacement == kBionicStackGuard ? reinterpret_cast<uintptr_t>(NtCurrentTeb())
                                                     : TlsCurrentThreadPointer();
  if (!value) return false;  // A missing Android TLS block remains an actual fault.
  CONTEXT* context = info->ContextRecord;
  DWORD64* registers[] = {&context->Rax, &context->Rcx, &context->Rdx, &context->Rbx,
      &context->Rsp, &context->Rbp, &context->Rsi, &context->Rdi, &context->R8, &context->R9,
      &context->R10, &context->R11, &context->R12, &context->R13, &context->R14, &context->R15};
  *registers[in.reg()] = value;
  context->Rip += in.length;  // MOV preserves flags and every other register.
  return true;
}
#endif

LONG CALLBACK FsVectoredHandler(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
    // A raw `syscall` the rewrite turned into ud2: emulated.
    return HandleSyscallTrap(info) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
  }
  if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2 &&
      info->ExceptionRecord->ExceptionInformation[0] == 8 /* execute */ &&
      HandleExecuteFault(info->ExceptionRecord->ExceptionInformation[1])) {
    // The first execution in a page of code made at run time: rewritten now.
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_BREAKPOINT) {
    auto addr = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    AcquireSRWLockShared(&g_lock);
    const TrapSite* site = FindTrapLocked(addr);
    TrapSite copy{};
    if (site != nullptr) copy = *site;
    ReleaseSRWLockShared(&g_lock);
    if (site == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (copy.stub != 0) {
#if defined(__x86_64__)
      info->ContextRecord->Rip = copy.stub;
#elif defined(__aarch64__)
      info->ContextRecord->Pc = copy.stub;
#endif
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
    // An execute fault's %rip is the address that cannot run: nothing to decode.
    if (info->ExceptionRecord->NumberParameters >= 2 &&
        info->ExceptionRecord->ExceptionInformation[0] == 8) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    // Recover bounded MOV reads missed by static code metadata. Other faults
    // retain their original diagnostic and exception handling.
#if defined(__x86_64__)
    uintptr_t rip = info->ContextRecord->Rip;
#elif defined(__aarch64__)
    uintptr_t rip = info->ContextRecord->Pc;
#endif
    AcquireSRWLockShared(&g_lock);
    const CodeRange* range = FindRangeLocked(rip);
    CodeRange r{};
    if (range != nullptr) r = *range;
    ReleaseSRWLockShared(&g_lock);
    if (range == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    X86Insn in;
    size_t avail = std::min<uintptr_t>(15, r.end - rip);
    bool decoded = DecodeX86(reinterpret_cast<const uint8_t*>(rip), avail, &in);
#if defined(__x86_64__)
    if (decoded && RecoverUncoveredMov(info, in, r)) return EXCEPTION_CONTINUE_EXECUTION;
#endif
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
  bool writable_executable = false;
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
// `readable` (vaddrs) is where the object's file bytes are mapped: a packed
// object's table can point outside them, into pages that are not mapped yet.
bool RegionsFromEhFrameHdr(const uint8_t* hdr, uint8_t* load_bias,
                           const std::vector<Region>& readable, std::vector<Region>* out) {
  const auto bias_value = reinterpret_cast<uintptr_t>(load_bias);
  auto in_file = [&](const uint8_t* p, size_t n) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p) - bias_value;
    for (const Region& r : readable) {
      if (v >= r.start && v <= r.end && n <= r.end - v) return true;
    }
    return false;
  };
  if (!in_file(hdr, 12) || hdr[0] != 1) return false;
  uint8_t eh_ptr_enc = hdr[1], count_enc = hdr[2], table_enc = hdr[3];
  const uint8_t* p = hdr + 4;
  uint64_t v;
  if (eh_ptr_enc == 0xFF || !ReadEncodedValue(&p, eh_ptr_enc, &v)) return false;
  if (count_enc == 0xFF || !ReadEncodedValue(&p, count_enc, &v)) return false;
  uint64_t count = v;
  if (table_enc != 0x3B || count == 0) return false;  // datarel | sdata4
  if (count > (1u << 24) || !in_file(p, static_cast<size_t>(count) * 8)) return false;
  const auto bias = reinterpret_cast<uintptr_t>(load_bias);
  const auto hdr_addr = reinterpret_cast<uintptr_t>(hdr);
  for (uint64_t i = 0; i < count; ++i, p += 8) {
    uintptr_t pc = hdr_addr + static_cast<intptr_t>(ReadI32(p));
    const uint8_t* fde = reinterpret_cast<const uint8_t*>(hdr_addr +
                                                          static_cast<intptr_t>(ReadI32(p + 4)));
    if (!in_file(fde, 32)) continue;
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
    if (!in_file(cie_field - cie_off, 32)) continue;
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


// --- control flow, shared by the load-time and the deferred rewrite ----------

// The target of a direct (relative) jump, call or conditional branch at
// `address` (whose bytes are at `at`), if `in` is one.
bool BranchTarget(const uint8_t* at, uint64_t address, const X86Insn& in, uint64_t* target) {
  bool relative = !in.vex && ((in.map == 0 &&
      (in.opcode == 0xE8 || in.opcode == 0xE9 || in.opcode == 0xEB ||
       (in.opcode >= 0x70 && in.opcode <= 0x7F) || (in.opcode >= 0xE0 && in.opcode <= 0xE3))) ||
      (in.map == 1 && in.opcode >= 0x80 && in.opcode <= 0x8F));
  if (!relative || (in.imm_size != 1 && in.imm_size != 4)) return false;
  const uint8_t* immediate = at + in.length - in.imm_size;
  int32_t delta;
  if (in.imm_size == 1) delta = static_cast<int8_t>(*immediate);
  else std::memcpy(&delta, immediate, sizeof delta);
  *target = address + in.length + delta;
  return true;
}

// Do not decode inline data after returns, unconditional jumps, padding
// traps or indirect jumps. Conditional edges retain their fallthrough.
bool EndsBlock(const X86Insn& in) {
  return !in.vex && ((in.map == 0 &&
      (in.opcode == 0xC3 || in.opcode == 0xC2 || in.opcode == 0xCB || in.opcode == 0xCA ||
       in.opcode == 0xE9 || in.opcode == 0xEB || in.opcode == 0xCC ||
       (in.opcode == 0xFF && (in.reg() & 7) >= 4 && (in.reg() & 7) <= 5))) ||
      (in.map == 1 && in.opcode == 0x0B));
}

bool IsSyscall(const uint8_t* site, const X86Insn& in) {
  return !in.vex && in.map == 1 && in.opcode == 0x05 && in.length == in.opcode_off + 2 &&
         site[in.opcode_off] == 0x0F && site[in.opcode_off + 1] == 0x05;
}

// --- objects whose code can change after the load (ADR 0084) -----------------

constexpr uintptr_t kPage = 0x1000;
uintptr_t PageOf(uintptr_t a) { return a & ~(kPage - 1); }

std::atomic<RawSyscallHandler> g_raw_syscall{nullptr};

struct LazyObject {
  const char* name = nullptr;  // owned with this object's registrations
  uintptr_t bias = 0;          // load bias
  uintptr_t lo = 0, hi = 0;    // the reservation
  std::vector<Region> fdes;    // code ranges from .eh_frame_hdr (vaddrs)
  bool unpacking = false;      // self-unpacking: its file bytes are not its code
  bool anonymous = false;      // code a raw system call mapped outside every object
  uint64_t eh_frame_hdr = 0;   // vaddr of PT_GNU_EH_FRAME, if any
  bool eh_frame_hdr_read = false;  // read from memory after unpacking
  std::vector<StubArena> arenas;         // all load-time and deferred rewrite stubs
  std::map<uintptr_t, uint8_t> patched;  // rewritten instruction -> its length
  std::map<uintptr_t, int> done;         // page rewritten on execution -> its protection
};

struct PendingPage {
  LazyObject* object;
  int prot;          // what loaded code asked for (with PROT_EXEC)
  bool regenerated;  // made executable after the load: its FDEs describe it
};

// g_lazy_lock guards the objects and pages; it is taken before g_lock, never
// while holding it, and never by the handler for anything but an execute fault.
SRWLOCK g_lazy_lock = SRWLOCK_INIT;
std::vector<LazyObject*>& Objects() {
  static std::vector<LazyObject*> v;
  return v;
}
std::map<uintptr_t, PendingPage>& Pending() {
  static std::map<uintptr_t, PendingPage> m;
  return m;
}
// Every rewritten `syscall` (now ud2) -> its length. Under g_lock.
std::map<uintptr_t, uint8_t>& SyscallSites() {
  static std::map<uintptr_t, uint8_t> m;
  return m;
}

// Code that copies itself carries our ud2 along: UPX's stub copies itself
// into an anonymous mapping and runs the copy. Each rewritten site keeps the
// bytes around it (as rewritten, within its page); a ud2 elsewhere with the
// same bytes around it is a copy of that system call. Under g_lock.
struct SyscallFingerprint {
  const LazyObject* owner;
  uint8_t length;           // the instruction's
  uint8_t before, after;    // bytes kept on each side of it
  uint8_t bytes[15 + 32];
};
std::vector<SyscallFingerprint>& Fingerprints() {
  static std::vector<SyscallFingerprint> v;
  return v;
}
constexpr size_t kFingerprintSide = 16;
constexpr size_t kFingerprintMin = 12;  // context bytes needed to call it a copy

bool ReadSafely(uintptr_t addr, void* out, size_t n) {
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(addr), out, n, &got) &&
         got == n;
}

// The length of the system call a ud2 at `rip` is a copy of, or 0: the same
// instruction bytes, and at least kFingerprintMin bytes around them that
// match outwards from it (a copy may end soon after: UPX copies its stub up
// to its last `ret`). With g_lock held.
//
// The bytes around `rip` are read once, a page at a time outwards from it
// (where a page is unreadable the window stops), and every fingerprint is
// compared in memory. Reading them per fingerprint and per byte cost a system
// call each: with Ijiami's many sites, the rewrite of one page on execution
// held g_lazy_lock for minutes, and a thread ART counted as runnable waited
// in mprotect until ART's suspend timeout aborted the process (ADR 0086).
uint8_t CopiedSyscallLengthLocked(uintptr_t rip) {
  if (Fingerprints().empty() || rip < kFingerprintSide) return 0;
  constexpr size_t kBack = kFingerprintSide;
  uint8_t window[kFingerprintSide + 15 + kFingerprintSide];
  size_t lo = kBack, hi = kBack;  // window[lo, hi) holds rip - kBack + lo ...
  for (uintptr_t a = rip; hi < sizeof(window);) {
    const size_t n = std::min<uintptr_t>(sizeof(window) - hi, PageOf(a) + kPage - a);
    if (!ReadSafely(a, window + hi, n)) break;
    hi += n;
    a += n;
  }
  for (uintptr_t a = rip; lo > 0;) {
    const size_t n = std::min<uintptr_t>(lo, a - PageOf(a - 1));
    if (!ReadSafely(a - n, window + lo - n, n)) break;
    lo -= n;
    a -= n;
  }
  for (const SyscallFingerprint& f : Fingerprints()) {
    if (hi - kBack < f.length || std::memcmp(window + kBack, f.bytes + f.before, f.length) != 0) {
      continue;
    }
    size_t back = 0, forward = 0;
    while (back < f.before && kBack - 1 - back >= lo &&
           window[kBack - 1 - back] == f.bytes[f.before - 1 - back]) {
      ++back;
    }
    while (forward < f.after && kBack + f.length + forward < hi &&
           window[kBack + f.length + forward] == f.bytes[f.before + f.length + forward]) {
      ++forward;
    }
    if (back + forward >= kFingerprintMin) return f.length;
  }
  return 0;
}

LazyObject* FindObjectLocked(uintptr_t addr) {
  for (LazyObject* o : Objects()) {
    if (addr >= o->lo && addr < o->hi) return o;
  }
  return nullptr;
}

// The protection a pending page has: what was asked for, without PROT_EXEC,
// and readable so that the rewrite can decode it.
int PendingProt(int prot) { return (prot & ~PROT_EXEC) | PROT_READ; }

// --- the rewriting ------------------------------------------------------------

// Rewrites single instructions of one object: at load time over its code, or
// on execution over code that was made at run time (`deferred`).
class Rewriter {
 public:
  Rewriter(LazyObject* object, bool deferred, FsRewriteStats* stats, std::string* error,
           size_t executable_tls_size, uintptr_t span_lo, uintptr_t span_hi,
           std::map<uintptr_t, int>* restore)
      : o_(object), deferred_(deferred), stats_(stats), error_(error),
        executable_tls_size_(executable_tls_size), span_lo_(span_lo), span_hi_(span_hi),
        restore_(restore), first_new_arena_(object->arenas.size()) {}

  // Rewrites the instruction `in` at `site` if it needs it. False only on a
  // hard failure (*error set).
  bool Rewrite(uint8_t* site, const X86Insn& in);
  // Makes the stubs executable and registers traps, system calls and the
  // given code ranges with the handler.
  bool Finish(const std::vector<CodeRange>& code_ranges);

 private:
  bool NeedSlot();
  uint8_t* AllocStub(size_t n);
  void AddTrap(uint8_t* site, const X86Insn& in, uintptr_t stub, const char* reason);
  bool MakeWritable(uint8_t* site, size_t n);
  void Unsupported(uint8_t* site, const X86Insn& in, const char* reason);
  void EnterStub(uint8_t* site, const X86Insn& in, uint8_t* stub);
  void NotePatched(const uint8_t* site, const X86Insn& in) {
    o_->patched[reinterpret_cast<uintptr_t>(site)] = static_cast<uint8_t>(in.length);
  }

  LazyObject* o_;
  bool deferred_;
  FsRewriteStats* stats_;
  std::string* error_;
  size_t executable_tls_size_;
  uintptr_t span_lo_, span_hi_;
  std::map<uintptr_t, int>* restore_;  // deferred: pages to re-protect afterwards
  int32_t slot_ = 0;                   // TEB offset of the thread-pointer slot
  size_t first_new_arena_;             // deferred: the object's arenas from here are new
  std::vector<TrapSite> traps_;
  std::vector<std::pair<uintptr_t, uint8_t>> syscalls_;
  std::set<uintptr_t> writable_pages_;
};

bool Rewriter::NeedSlot() {
  if (slot_ == 0) slot_ = static_cast<int32_t>(TlsThreadPointerSlot());
  if (slot_ == 0) {
    if (error_) *error_ = std::string(o_->name) + ": no inline TLS slot for the %fs rewrite";
    return false;
  }
  return true;
}

uint8_t* Rewriter::AllocStub(size_t n) {
  std::vector<StubArena>& arenas = o_->arenas;
  if (arenas.empty() || arenas.back().used + n > kStubChunk ||
      (deferred_ && !arenas.back().writable_executable)) {
    uint8_t* chunk = AllocNear(span_lo_, span_hi_);
    if (chunk == nullptr) return nullptr;
    if (deferred_) {
      // Stubs of deferred rewrites are added while other threads run earlier
      // ones: their memory stays writable and executable.
      DWORD old;
      if (!VirtualProtect(chunk, kStubChunk, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(chunk, 0, MEM_RELEASE);
        return nullptr;
      }
    }
    arenas.push_back({chunk, 0, deferred_});
  }
  uint8_t* p = arenas.back().base + arenas.back().used;
  arenas.back().used += n;
  return p;
}

void Rewriter::AddTrap(uint8_t* site, const X86Insn& in, uintptr_t stub, const char* reason) {
  TrapSite t{};
  t.addr = reinterpret_cast<uintptr_t>(site);
  t.stub = stub;
  t.reason = reason;
  t.lib = o_->name;
  t.lib_bias = o_->bias;
  t.length = static_cast<uint8_t>(in.length);
  std::memcpy(t.bytes, site, in.length);
  traps_.push_back(t);
  NotePatched(site, in);
  site[0] = 0xCC;  // int3
}

// At load time the segments are file-backed views mapped read-execute
// (elf_loader.cc, MapSegments): only the pages that get patched become
// writable (copy-on-write, so they alone turn private); the loader's final
// mprotect makes them read-execute again. Deferred, a page is pending (not
// executable) or was rewritten before (executable, maybe running on another
// thread): it is made writable, keeping that, and its protection is restored
// afterwards (restore_).
bool Rewriter::MakeWritable(uint8_t* site, size_t n) {
  const uintptr_t first = reinterpret_cast<uintptr_t>(site) & ~(kPage - 1);
  const uintptr_t last = (reinterpret_cast<uintptr_t>(site) + n - 1) & ~(kPage - 1);
  for (uintptr_t page = first; page <= last; page += kPage) {
    if (writable_pages_.count(page) != 0) continue;
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    if (deferred_) {
      int restore;
      auto pending = Pending().find(page);
      if (pending != Pending().end()) {
        restore = PendingProt(pending->second.prot);
        prot = PROT_READ | PROT_WRITE;
      } else if (auto done = o_->done.find(page); done != o_->done.end()) {
        restore = done->second;
        prot = restore | PROT_READ | PROT_WRITE;
      } else {
        restore = PROT_READ | PROT_EXEC;  // not reached: the walk stays in such pages
      }
      (*restore_)[page] = restore;
    }
    if (mprotect(reinterpret_cast<void*>(page), kPage, prot) != 0) {
      if (error_) *error_ = std::string(o_->name) + ": cannot make code writable for the %fs rewrite";
      return false;
    }
    writable_pages_.insert(page);
  }
  return true;
}

void Rewriter::Unsupported(uint8_t* site, const X86Insn& in, const char* reason) {
  std::fprintf(stderr,
               "gesher elf: %s+0x%llx: %%fs access not rewritten (%s); it aborts if reached\n",
               o_->name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site) - o_->bias),
               reason);
  AddTrap(site, in, 0, reason);
  ++stats_->trapped;
}

// The site becomes a `jmp rel32` to the stub when the instruction is long
// enough and the stub is within reach, else an int3 the handler redirects.
void Rewriter::EnterStub(uint8_t* site, const X86Insn& in, uint8_t* stub) {
  const uintptr_t stub_addr = reinterpret_cast<uintptr_t>(stub);
  const uintptr_t next = reinterpret_cast<uintptr_t>(site) + 5;
  if (in.length >= 5 && Rel32Reachable(next, stub_addr)) {
    NotePatched(site, in);
    site[0] = 0xE9;  // jmp rel32
    WriteI32(site + 1, static_cast<int32_t>(static_cast<int64_t>(stub_addr) -
                                            static_cast<int64_t>(next)));
    for (size_t i = 5; i < in.length; ++i) site[i] = 0xCC;
  } else {
    AddTrap(site, in, stub_addr, nullptr);
  }
}

bool Rewriter::Rewrite(uint8_t* site, const X86Insn& in) {
  if (IsSyscall(site, in)) {
    // A raw Linux system call: ud2 (same length), emulated by the handler.
    if (!MakeWritable(site + in.opcode_off + 1, 1)) return false;
    NotePatched(site, in);
    site[in.opcode_off + 1] = 0x0B;
    syscalls_.emplace_back(reinterpret_cast<uintptr_t>(site), static_cast<uint8_t>(in.length));
    ++stats_->syscalls;
    return true;
  }
  if (!in.fs || !in.has_mem()) return true;  // %fs on a non-memory form is inert
  if (in.map == 0 && in.opcode == 0x8D) return true;  // lea ignores the segment
  if (in.map == 1 && !in.vex && (in.opcode >= 0x18 && in.opcode <= 0x1F)) return true;  // hints/nop
  if (in.map == 1 && !in.vex && in.opcode == 0x0D) return true;  // prefetchw
  // Every path below changes the instruction's bytes.
  if (!MakeWritable(site, in.length)) return false;
  if (in.gs) {
    Unsupported(site, in, "both %fs and %gs prefixes");
    return true;
  }
  if (in.absolute()) {
    int32_t disp = ReadI32(site + in.disp_off);
    if (disp < 0 && static_cast<uint64_t>(-static_cast<int64_t>(disp)) <= executable_tls_size_) {
      int scratch = -1;
      if (const char* why = PlanAbsoluteStub(in, &scratch)) {
        Unsupported(site, in, why);
        return true;
      }
      if (!NeedSlot()) return false;
      uint8_t* stub = AllocStub(kMaxStub);
      if (!stub) {
        if (error_) *error_ = std::string(o_->name) + ": out of memory for local-exec TLS stubs";
        return false;
      }
      uint8_t* tail = EmitAbsoluteStub(stub, site, in, scratch, slot_);
      const uint8_t jump[] = {0xFF, 0x25, 0, 0, 0, 0};
      std::memcpy(tail, jump, sizeof(jump));
      uint64_t back = reinterpret_cast<uint64_t>(site + in.length);
      std::memcpy(tail + sizeof(jump), &back, 8);
      ++stats_->stubbed;
      EnterStub(site, in, stub);
      return true;
    }
    int32_t to;
    if (disp == kBionicStackGuard) {
      to = kTebSelf;
      ++stats_->stack_guard;
    } else if (disp == 0) {
      if (!NeedSlot()) return false;
      to = slot_;
      ++stats_->thread_ptr;
    } else {
      Unsupported(site, in, "a %fs displacement other than 0 (thread pointer) or 0x28 (guard)");
      return true;
    }
    for (size_t i = 0; i < in.opcode_off; ++i) {
      if (site[i] == 0x64) site[i] = 0x65;
    }
    WriteI32(site + in.disp_off, to);
    return true;
  }
  if (in.rip_relative()) {
    Unsupported(site, in, "a RIP-relative %fs operand");
    return true;
  }
  int adj = -1;
  bool restore = true;
  if (const char* why = PlanStub(in, &adj, &restore)) {
    Unsupported(site, in, why);
    return true;
  }
  if (!NeedSlot()) return false;
  uint8_t* stub = AllocStub(kMaxStub);
  if (stub == nullptr) {
    if (error_) *error_ = std::string(o_->name) + ": out of memory for %fs stubs";
    return false;
  }
  uint8_t* s = EmitAdjust(stub, adj, true, slot_);
  for (size_t i = 0; i < in.length; ++i) {  // the instruction without 0x64
    if (i < in.opcode_off && site[i] == 0x64) continue;
    *s++ = site[i];
  }
  if (restore) s = EmitAdjust(s, adj, false, slot_);
  const uint8_t jmp_abs[] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp *0(%rip)
  std::memcpy(s, jmp_abs, sizeof(jmp_abs));
  s += sizeof(jmp_abs);
  uint64_t back = reinterpret_cast<uint64_t>(site + in.length);
  std::memcpy(s, &back, 8);
  ++stats_->stubbed;
  EnterStub(site, in, stub);
  return true;
}

bool Rewriter::Finish(const std::vector<CodeRange>& code_ranges) {
  if (!deferred_) {
    // Make the stubs executable.
    for (const StubArena& a : o_->arenas) {
      DWORD old;
      if (!VirtualProtect(a.base, kStubChunk, PAGE_EXECUTE_READ, &old)) {
        if (error_) *error_ = std::string(o_->name) + ": cannot make %fs stubs executable";
        return false;
      }
      FlushInstructionCache(GetCurrentProcess(), a.base, kStubChunk);
    }
  } else {
    for (size_t i = 0; i < o_->arenas.size(); ++i) {
      FlushInstructionCache(GetCurrentProcess(), o_->arenas[i].base, kStubChunk);
    }
  }
  AcquireSRWLockExclusive(&g_lock);
  EnsureHandler();
  auto& all = Traps();
  // A page can be rewritten more than once. Replace a previous trap at the
  // same instruction address so the handler cannot choose an obsolete stub.
  // Other existing traps still describe unchanged int3 bytes in that page.
  std::set<uintptr_t> replaced;
  for (const TrapSite& trap : traps_) replaced.insert(trap.addr);
  all.erase(std::remove_if(all.begin(), all.end(),
      [&](const TrapSite& trap) { return replaced.count(trap.addr) != 0; }), all.end());
  all.insert(all.end(), traps_.begin(), traps_.end());
  std::sort(all.begin(), all.end(),
            [](const TrapSite& a, const TrapSite& b) { return a.addr < b.addr; });
  for (const auto& site : syscalls_) {
    SyscallSites()[site.first] = site.second;
    // Its bytes as they are now that every rewrite of this pass is done.
    SyscallFingerprint f{};
    f.owner = o_;
    f.length = site.second;
    const uintptr_t page = PageOf(site.first);
    const uintptr_t end = site.first + site.second;
    f.before = static_cast<uint8_t>(std::min<uintptr_t>(kFingerprintSide, site.first - page));
    f.after = static_cast<uint8_t>(end >= page + kPage ? 0
                                   : std::min<uintptr_t>(kFingerprintSide, page + kPage - end));
    const size_t kept = static_cast<size_t>(f.before) + f.after;
    if (kept >= kFingerprintMin &&
        ReadSafely(site.first - f.before, f.bytes, kept + f.length)) {
      Fingerprints().push_back(f);
    }
  }
  for (const CodeRange& r : code_ranges) Ranges().push_back(r);
  const std::vector<StubArena>& arenas = o_->arenas;
  for (size_t i = first_new_arena_; i < arenas.size(); ++i) {
    Ranges().push_back({reinterpret_cast<uintptr_t>(arenas[i].base),
                        reinterpret_cast<uintptr_t>(arenas[i].base) + kStubChunk, o_->name,
                        o_->bias, true});
  }
  ReleaseSRWLockExclusive(&g_lock);
  return true;
}

// --- deferred rewrite, on execution -------------------------------------------

// A `syscall` rewritten before (now ud2, which would end the walk), or 0.
uint8_t KnownSyscallLength(uintptr_t address) {
  AcquireSRWLockShared(&g_lock);
  auto& sites = SyscallSites();
  auto it = sites.find(address);
  uint8_t length = it != sites.end() ? it->second : 0;
  if (length == 0) length = CopiedSyscallLengthLocked(address);
  ReleaseSRWLockShared(&g_lock);
  const auto* bytes = reinterpret_cast<const uint8_t*>(address);
  if (length < 2 || bytes[length - 2] != 0x0F || bytes[length - 1] != 0x0B) return 0;
  return length;
}

// The readable memory of `o`'s reservation now, as vaddrs.
std::vector<Region> ReadableRegions(const LazyObject* o) {
  std::vector<Region> out;
  for (uintptr_t at = o->lo; at < o->hi;) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) break;
    const uintptr_t end =
        std::min(o->hi, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
    if (end <= at) break;
    const bool readable = info.State == MEM_COMMIT &&
                          (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    if (readable) {
      if (!out.empty() && out.back().end == at - o->bias) out.back().end = end - o->bias;
      else out.push_back({at - o->bias, end - o->bias});
    }
    at = end;
  }
  return out;
}

// Rewrites the code reachable from `entry`, the address the first execution in
// a pending page faulted at, along direct control flow, and from the starts of
// the FDEs in that page once its code is regenerated. Decoding stays in the
// object's pending and rewritten pages. With g_lazy_lock held exclusively.
bool RewriteOnExecution(LazyObject* o, uintptr_t entry, bool regenerated,
                        std::map<uintptr_t, int>* restore) {
  auto& pending = Pending();
  // A packed object's .eh_frame_hdr may exist only once it is unpacked.
  if (regenerated && o->fdes.empty() && o->eh_frame_hdr != 0 && !o->eh_frame_hdr_read) {
    o->eh_frame_hdr_read = true;
    RegionsFromEhFrameHdr(reinterpret_cast<const uint8_t*>(o->bias + o->eh_frame_hdr),
                          reinterpret_cast<uint8_t*>(o->bias), ReadableRegions(o), &o->fdes);
  }
  auto walkable_page = [&](uintptr_t page) {
    if (page < o->lo || page >= o->hi) return false;
    auto it = pending.find(page);
    if (it != pending.end()) return it->second.object == o;
    return o->done.count(page) != 0;
  };
  // FDEs describe a self-unpacking object's code only where it was unpacked.
  auto fde_page = [&](uintptr_t page) {
    if (!walkable_page(page)) return false;
    if (!o->unpacking) return true;
    auto it = pending.find(page);
    return it != pending.end() ? it->second.regenerated : true;
  };
  auto limit = [&](uintptr_t address) {
    uintptr_t page = PageOf(address);
    while (walkable_page(page)) page += kPage;
    return page;
  };

  std::vector<uintptr_t> roots{entry};
  if (regenerated || !o->unpacking) {
    const uintptr_t page = PageOf(entry);
    for (const Region& r : o->fdes) {
      const uintptr_t start = o->bias + r.start, end = o->bias + r.end;
      if (start < page + kPage && end > page && fde_page(PageOf(start))) roots.push_back(start);
    }
  }

  FsRewriteStats stats;  // deferred rewrites are not counted in the load's
  std::string error;
  Rewriter rewriter(o, true, &stats, &error, 0, o->lo, o->hi, restore);
  std::set<uintptr_t> visited;
  bool rewritten = true;
  for (size_t next = 0; rewritten && next < roots.size(); ++next) {
    uintptr_t address = roots[next];
    const uintptr_t end = limit(address);
    while (address < end && visited.insert(address).second) {
      // An instruction rewritten before: its original continues after it.
      auto patched = o->patched.find(address);
      if (patched != o->patched.end()) {
        address += patched->second;
        continue;
      }
      if (const uint8_t length = KnownSyscallLength(address)) {
        address += length;
        continue;
      }
      X86Insn in;
      const auto* at = reinterpret_cast<uint8_t*>(address);
      if (!DecodeX86(at, static_cast<size_t>(end - address), &in)) break;
      uint64_t target;
      if (BranchTarget(at, address, in, &target) && walkable_page(PageOf(target))) {
        roots.push_back(static_cast<uintptr_t>(target));
      }
      if (!rewriter.Rewrite(reinterpret_cast<uint8_t*>(address), in)) {
        std::fprintf(stderr, "gesher elf: %s\n", error.c_str());
        rewritten = false;
        break;
      }
      if (EndsBlock(in)) break;
      address += in.length;
    }
  }
  // Register any instructions already patched even after a later failure,
  // but do not let the caller execute a partially rewritten page.
  return rewriter.Finish({}) && rewritten;
}

bool HandleExecuteFault(uintptr_t addr) {
  const uintptr_t page = PageOf(addr);
  AcquireSRWLockExclusive(&g_lazy_lock);
  bool handled = false;
  auto& pending = Pending();
  auto it = pending.find(page);
  if (it == pending.end()) {
    // Another thread rewrote it first: run it, if it is executable now.
    LazyObject* o = FindObjectLocked(addr);
    if (o != nullptr) {
      auto done = o->done.find(page);
      handled = done != o->done.end() && (done->second & PROT_EXEC) != 0;
    }
  } else {
    const PendingPage p = it->second;
    std::map<uintptr_t, int> restore;
    bool rewritten = RewriteOnExecution(p.object, addr, p.regenerated, &restore);
    for (const auto& r : restore) {
      if (mprotect(reinterpret_cast<void*>(r.first), kPage, r.second) != 0) rewritten = false;
    }
    if (rewritten && mprotect(reinterpret_cast<void*>(page), kPage, p.prot) == 0) {
      FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(page), kPage);
      pending.erase(page);
      p.object->done[page] = p.prot;
      handled = true;
    }
  }
  ReleaseSRWLockExclusive(&g_lazy_lock);
  return handled;
}

bool HandleSyscallTrap(EXCEPTION_POINTERS* info) {
#if defined(__x86_64__)
  CONTEXT* c = info->ContextRecord;
  const uintptr_t rip = c->Rip;
  AcquireSRWLockShared(&g_lock);
  auto& sites = SyscallSites();
  auto it = sites.find(rip);
  uint8_t length = it != sites.end() ? it->second : 0;
  const bool copied = length == 0 && (length = CopiedSyscallLengthLocked(rip)) != 0;
  ReleaseSRWLockShared(&g_lock);
  if (length < 2) return false;
  if (copied) {  // known from now on
    AcquireSRWLockExclusive(&g_lock);
    SyscallSites()[rip] = length;
    ReleaseSRWLockExclusive(&g_lock);
  }
  // Only our own ud2: code written over the site since is something else.
  const auto* bytes = reinterpret_cast<const uint8_t*>(rip);
  if (bytes[length - 2] != 0x0F || bytes[length - 1] != 0x0B) return false;
  const int64_t args[6] = {static_cast<int64_t>(c->Rdi), static_cast<int64_t>(c->Rsi),
                           static_cast<int64_t>(c->Rdx), static_cast<int64_t>(c->R10),
                           static_cast<int64_t>(c->R8),  static_cast<int64_t>(c->R9)};
  RawSyscallHandler handler = g_raw_syscall.load(std::memory_order_acquire);
  constexpr int64_t kENOSYS = 38;
  const int64_t result =
      handler != nullptr ? handler(static_cast<int64_t>(c->Rax), args) : -kENOSYS;
  // As the kernel returns: the result in %rax, %rcx = next %rip, %r11 = rflags.
  c->Rax = static_cast<DWORD64>(result);
  c->Rcx = rip + length;
  c->R11 = c->EFlags;
  c->Rip = rip + length;
  return true;
#else
  (void)info;
  return false;
#endif
}

}  // namespace

FsRewriteRegistration::~FsRewriteRegistration() {
  auto* object = static_cast<LazyObject*>(object_);
  // The loader has not exposed this image or run its constructors. Its owner
  // may have executed an IFUNC, but that call has returned before rollback.
  // Use the same lock order as deferred rewriting and withdraw every pointer
  // before releasing names, stubs or the caller's ELF mapping.
  AcquireSRWLockExclusive(&g_lazy_lock);
  auto& objects = Objects();
  objects.erase(std::remove(objects.begin(), objects.end(), object), objects.end());
  for (auto it = Pending().begin(); it != Pending().end();) {
    if (it->second.object == object) it = Pending().erase(it);
    else ++it;
  }
  AcquireSRWLockExclusive(&g_lock);
  auto& traps = Traps();
  traps.erase(std::remove_if(traps.begin(), traps.end(),
      [&](const TrapSite& t) { return t.lib == object->name; }), traps.end());
  auto& ranges = Ranges();
  ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
      [&](const CodeRange& r) { return r.lib == object->name; }), ranges.end());
  auto& sites = SyscallSites();
  sites.erase(sites.lower_bound(object->lo), sites.lower_bound(object->hi));
  auto& fingerprints = Fingerprints();
  fingerprints.erase(std::remove_if(fingerprints.begin(), fingerprints.end(),
      [&](const SyscallFingerprint& f) { return f.owner == object; }), fingerprints.end());
  ReleaseSRWLockExclusive(&g_lock);
  ReleaseSRWLockExclusive(&g_lazy_lock);
  for (const StubArena& arena : object->arenas) VirtualFree(arena.base, 0, MEM_RELEASE);
  std::free(const_cast<char*>(object->name));
  delete object;
}

bool RewriteFsAccesses(const char* name, const uint8_t* file, size_t file_size,
                       uint8_t* load_bias, uint8_t* map_base, size_t map_size,
                       FsRewriteStats* stats, std::string* error,
                       size_t executable_tls_size,
                       std::unique_ptr<FsRewriteRegistration>* registration) {
  *stats = FsRewriteStats{};
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(file + ehdr->e_phoff);
  const auto bias = reinterpret_cast<uintptr_t>(load_bias);

  // Executable file-backed ranges, the file bytes of every segment, and
  // .eh_frame_hdr (only where the file's bytes are mapped: a packed object's
  // can point into its unpacked image, which is not there yet).
  std::vector<Region> exec, file_bytes;
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type == PT_LOAD && phdr[i].p_filesz > 0) {
      file_bytes.push_back({phdr[i].p_vaddr, phdr[i].p_vaddr + phdr[i].p_filesz});
      if (phdr[i].p_flags & PF_X) exec.push_back(file_bytes.back());
    }
  }
  const uint8_t* eh_frame_hdr = nullptr;
  uint64_t eh_frame_hdr_vaddr = 0;
  bool eh_frame_hdr_outside = false;
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_GNU_EH_FRAME) continue;
    bool inside = false;
    for (const Region& r : file_bytes) {
      inside |= phdr[i].p_vaddr >= r.start && phdr[i].p_vaddr + 12 <= r.end;
    }
    if (inside) eh_frame_hdr = load_bias + phdr[i].p_vaddr;
    else eh_frame_hdr_outside = true;
    eh_frame_hdr_vaddr = phdr[i].p_vaddr;
  }

  // The object, for code made executable after the load. Registered when the
  // load-time rewrite is done.
  auto* object = new LazyObject();
  auto owner = std::make_unique<FsRewriteRegistration>(object);
  object->name = _strdup(name);
  if (object->name == nullptr) {
    if (error) *error = "cannot allocate rewrite registration name";
    return false;
  }
  object->bias = bias;
  object->lo = reinterpret_cast<uintptr_t>(map_base);
  object->hi = object->lo + map_size;
  object->eh_frame_hdr = eh_frame_hdr_vaddr;
  auto register_object = [&] {
    AcquireSRWLockExclusive(&g_lazy_lock);
    Objects().push_back(object);
    ReleaseSRWLockExclusive(&g_lazy_lock);
    if (registration != nullptr) *registration = std::move(owner);
    else owner.release();  // callers without an owner retain process lifetime
  };
  if (exec.empty()) {
    register_object();
    return true;
  }

  // Unwind coverage can be partial, but executable sections also contain data
  // (OpenSSL's assembly tables, for example). Never sweep gaps between FDEs as
  // instructions. Function symbols and direct control-flow edges supply roots
  // for decoding code without unwind metadata.
  std::vector<Region> regions, sections;
  std::vector<uint64_t> roots;
  if (eh_frame_hdr != nullptr &&
      RegionsFromEhFrameHdr(eh_frame_hdr, load_bias, file_bytes, &regions))
    stats->used_fdes = true;
  object->fdes = regions;

  // A self-unpacking object: its unwind information describes code that is
  // not in its executable file bytes (or is itself not in the file), or UPX
  // packed it. Its file bytes there are compressed data: nothing is rewritten
  // now; DeferUnpackedCode makes the code wait for its first execution.
  size_t outside = 0;
  for (const Region& r : regions) {
    bool in_exec = false;
    for (const Region& e : exec) in_exec |= r.start >= e.start && r.end <= e.end;
    if (!in_exec) ++outside;
  }
  static const char kUpxInfo[] = "$Info: This file is packed with the UPX";
  bool upx = false;
  for (int i = 0; i < ehdr->e_phnum && !upx; ++i) {
    if (phdr[i].p_type != PT_LOAD || !(phdr[i].p_flags & PF_X) ||
        phdr[i].p_offset > file_size || phdr[i].p_filesz > file_size - phdr[i].p_offset) continue;
    const uint8_t* begin = file + phdr[i].p_offset;
    const uint8_t* end = begin + phdr[i].p_filesz;
    upx = std::search(begin, end, kUpxInfo, kUpxInfo + sizeof(kUpxInfo) - 1) != end;
  }
  if (eh_frame_hdr_outside || upx || (outside > 0 && outside * 4 >= regions.size())) {
    stats->self_unpacking = true;
    object->unpacking = true;
    std::fprintf(stderr,
                 "gesher elf: %s unpacks its code at run time (%s); it is rewritten as it runs\n",
                 name, upx ? "UPX" : eh_frame_hdr_outside ? ".eh_frame_hdr outside its file bytes"
                                                          : "unwind information outside its code");
    // The whole reservation, for the handler's diagnostics and recovery.
    Rewriter rewriter(object, false, stats, error, executable_tls_size, object->lo, object->hi,
                      nullptr);
    if (!rewriter.Finish({{object->lo, object->hi, object->name, bias, false}})) return false;
    register_object();
    return true;
  }

  if (ehdr->e_entry != 0) roots.push_back(ehdr->e_entry);
  if (ehdr->e_shoff != 0 && ehdr->e_shentsize == sizeof(Elf64_Shdr) &&
      ehdr->e_shoff <= file_size &&
      uint64_t{ehdr->e_shnum} * sizeof(Elf64_Shdr) <= file_size - ehdr->e_shoff) {
    const auto* shdr = reinterpret_cast<const Elf64_Shdr*>(file + ehdr->e_shoff);
    for (int i = 0; i < ehdr->e_shnum; ++i) {
      if (shdr[i].sh_type == SHT_PROGBITS && (shdr[i].sh_flags & SHF_EXECINSTR) &&
          (shdr[i].sh_flags & SHF_ALLOC) && shdr[i].sh_size > 0 &&
          shdr[i].sh_addr <= UINT64_MAX - shdr[i].sh_size) {
        sections.push_back({shdr[i].sh_addr, shdr[i].sh_addr + shdr[i].sh_size});
      }
      if ((shdr[i].sh_type == SHT_SYMTAB || shdr[i].sh_type == SHT_DYNSYM) &&
          shdr[i].sh_entsize == sizeof(Elf64_Sym) && shdr[i].sh_offset <= file_size &&
          shdr[i].sh_size <= file_size - shdr[i].sh_offset) {
        const auto* symbols = reinterpret_cast<const Elf64_Sym*>(file + shdr[i].sh_offset);
        for (size_t j = 0; j < shdr[i].sh_size / sizeof(Elf64_Sym); j++) {
          if (ELF64_ST_TYPE(symbols[j].st_info) == STT_FUNC && symbols[j].st_shndx != SHN_UNDEF)
            roots.push_back(symbols[j].st_value);
        }
      }
    }
  }
  if (sections.empty()) sections = exec;
  // Preserve the historical fallback for objects with no usable code metadata.
  if (regions.empty() && roots.empty()) regions = sections;
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
  std::vector<Region> coverage;
  for (const Region& r : code) {
    if (!coverage.empty() && r.start <= coverage.back().end) {
      coverage.back().end = std::max(coverage.back().end, r.end);
    } else {
      coverage.push_back(r);
    }
  }
  auto covered = [&](uint64_t address) {
    auto at = std::upper_bound(coverage.begin(), coverage.end(), address,
        [](uint64_t value, const Region& r) { return value < r.start; });
    return at != coverage.begin() && address < std::prev(at)->end;
  };
  auto limit = [&](uint64_t address) -> uint64_t {
    for (const Region& s : sections) for (const Region& e : exec)
      if (address >= std::max(s.start, e.start) && address < std::min(s.end, e.end))
        return std::min(s.end, e.end);
    return address;
  };
  auto branch = [&](uint64_t address, const X86Insn& in) {
    uint64_t target;
    if (!BranchTarget(load_bias + address, address, in, &target)) return;
    if (!covered(target) && limit(target) > target) roots.push_back(target);
  };
  // Find references to uncovered code before any instruction bytes change.
  for (const Region& r : coverage) {
    for (uint64_t address = r.start; address < r.end;) {
      X86Insn in;
      if (!DecodeX86(load_bias + address, static_cast<size_t>(r.end - address), &in)) { ++address; continue; }
      branch(address, in);
      address += in.length;
    }
  }
  std::vector<Region> merged = coverage;
  std::set<uint64_t> visited;
  for (size_t next = 0; next < roots.size(); next++) {
    uint64_t address = roots[next], end = limit(address);
    while (address < end && !covered(address) && visited.insert(address).second) {
      X86Insn in;
      if (!DecodeX86(load_bias + address, static_cast<size_t>(end - address), &in)) break;
      merged.push_back({address, address + in.length});
      branch(address, in);
      if (EndsBlock(in)) break;
      address += in.length;
    }
  }

  // The object's whole executable span, for stub placement and diagnostics.
  uintptr_t span_lo = UINTPTR_MAX, span_hi = 0;
  for (const Region& e : exec) {
    span_lo = std::min<uintptr_t>(span_lo, bias + e.start);
    span_hi = std::max<uintptr_t>(span_hi, bias + e.end);
  }

  Rewriter rewriter(object, false, stats, error, executable_tls_size, span_lo, span_hi, nullptr);
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
      if (!rewriter.Rewrite(site, in)) return false;
    }
  }

  // The whole reservation: code made executable after the load is the
  // object's too (diagnostics, recovery of missed %fs reads).
  if (!rewriter.Finish({{object->lo, object->hi, object->name, bias, false}})) return false;
  register_object();
  return true;
}

bool DeferUnpackedCode(const char* name, const uint8_t* file, uint8_t* load_bias,
                       std::string* error) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(file);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(file + ehdr->e_phoff);
  const auto bias = reinterpret_cast<uintptr_t>(load_bias);
  AcquireSRWLockExclusive(&g_lazy_lock);
  bool ok = true;
  LazyObject* object = nullptr;
  for (LazyObject* o : Objects()) {
    if (o->bias == bias) object = o;
  }
  for (int i = 0; ok && object != nullptr && i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD || !(phdr[i].p_flags & PF_X) || phdr[i].p_memsz == 0) continue;
    const uintptr_t start = PageOf(bias + phdr[i].p_vaddr);
    const uintptr_t end = PageOf(bias + phdr[i].p_vaddr + phdr[i].p_memsz + kPage - 1);
    const int prot = PROT_READ | PROT_EXEC | ((phdr[i].p_flags & PF_W) ? PROT_WRITE : 0);
    ok = mprotect(reinterpret_cast<void*>(start), end - start, PendingProt(prot)) == 0;
    for (uintptr_t page = start; ok && page < end; page += kPage) {
      Pending()[page] = {object, prot, false};
    }
  }
  ReleaseSRWLockExclusive(&g_lazy_lock);
  if (!ok && error) *error = std::string(name) + ": cannot defer the code of a self-unpacking object";
  return ok && object != nullptr;
}

// --- public: raw system calls and memory of loaded code (elf_loader.h) -------

void SetRawSyscallHandler(RawSyscallHandler handler) {
  g_raw_syscall.store(handler, std::memory_order_release);
}

namespace {

// For each run of pages in [start, end) inside a registered object (with
// g_lazy_lock held): `inside(lo, hi, object)`; for the rest, `outside(lo, hi)`.
template <typename Inside, typename Outside>
bool ForEachObjectRun(uintptr_t start, uintptr_t end, Inside inside, Outside outside) {
  bool ok = true;
  for (uintptr_t at = start; at < end;) {
    LazyObject* o = FindObjectLocked(at);
    uintptr_t next = end;
    if (o != nullptr) {
      next = std::min(end, o->hi);
      ok &= inside(at, next, o);
    } else {
      for (LazyObject* other : Objects()) {
        if (other->lo > at && other->lo < next) next = other->lo;
      }
      ok &= outside(at, next);
    }
    at = next;
  }
  return ok;
}

bool OverlapsObjectLocked(uintptr_t start, uintptr_t end) {
  for (LazyObject* o : Objects()) {
    if (o->lo < end && start < o->hi) return true;
  }
  return false;
}

// The pages' contents may change: what was rewritten there is decoded anew.
void ForgetPatchedLocked(LazyObject* o, uintptr_t lo, uintptr_t hi) {
  o->patched.erase(o->patched.lower_bound(PageOf(lo)), o->patched.lower_bound(hi));
}

// Pages that were unmapped, replaced or made non-executable are neither
// pending nor rewritten.
void ForgetPagesLocked(LazyObject* o, uintptr_t lo, uintptr_t hi) {
  for (uintptr_t page = PageOf(lo); page < hi; page += kPage) {
    Pending().erase(page);
    o->done.erase(page);
  }
  ForgetPatchedLocked(o, lo, hi);
}

// [lo, hi) of `o` now holds code to be run with `prot`: pending until its
// first execution.
void DeferPagesLocked(LazyObject* o, uintptr_t lo, uintptr_t hi, int prot) {
  for (uintptr_t page = PageOf(lo); page < hi; page += kPage) {
    o->done.erase(page);
    Pending()[page] = {o, prot, true};
  }
  ForgetPatchedLocked(o, lo, hi);
}

uintptr_t PageEnd(uintptr_t addr, size_t len) {
  const uintptr_t end = addr + len;
  return end < addr ? UINTPTR_MAX & ~(kPage - 1) : PageOf(end + kPage - 1);
}

// A native shared view owns the file object and its final partial page. In
// particular, bytes written after EOF in that page belong to the mapping's
// cache, not to the file's logical contents. Writing them with pwrite would
// extend the file; omitting them would lose them on a subsequent mmap.
struct SharedMapping {
  HANDLE section = nullptr;
  uint8_t* view = nullptr;
  uint8_t* data = nullptr;
  bool writable = false;
  // A non-writable page was synchronized before losing write access (or has
  // never been writable). Its backing is current, including with PROT_NONE.
  std::vector<bool> readonly_clean;
  ~SharedMapping() {
    if (view != nullptr) UnmapViewOfFile(view);
    if (section != nullptr) CloseHandle(section);
  }
};

// The live page ranges of copied shared mappings. Split ranges share one
// native backing view; descriptor reuse cannot change its identity. These
// copies publish changes at msync/unmap/replacement; they do not provide
// live coherence with another anonymous copy. Under g_lazy_lock.
struct SharedCopy {
  uintptr_t start = 0;
  size_t len = 0;  // whole mapped pages, including the partial EOF page
  size_t backing_offset = 0;
  std::shared_ptr<SharedMapping> backing;
};
std::vector<SharedCopy>& SharedCopies() {
  static std::vector<SharedCopy> v;
  return v;
}

bool OverlapsSharedCopyLocked(uintptr_t lo, uintptr_t hi) {
  for (const SharedCopy& copy : SharedCopies()) {
    if (copy.start < hi && lo < copy.start + copy.len) return true;
  }
  return false;
}

// Copy whole affected pages into the native view, including its EOF tail.
// ReadProcessMemory reports inaccessible source pages instead of crashing
// the host. No guest page protections are changed to perform writeback.
bool WriteBackLocked(uintptr_t lo, uintptr_t hi) {
  for (const SharedCopy& copy : SharedCopies()) {
    const uintptr_t a = std::max(lo, copy.start), b = std::min(hi, copy.start + copy.len);
    if (a >= b || !copy.backing->writable) continue;
    for (uintptr_t at = a; at < b;) {
      const size_t offset = copy.backing_offset + (at - copy.start);
      if (copy.backing->readonly_clean[offset / kPage]) { at += kPage; continue; }
      uintptr_t end = at + kPage;
      while (end < b && !copy.backing->readonly_clean[
                                  (copy.backing_offset + end - copy.start) / kPage]) end += kPage;
      uint8_t* target = copy.backing->data + offset;
      SIZE_T copied = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at),
                             target, end - at, &copied) || copied != end - at) {
        errno = EFAULT;
        return false;
      }
      if (!FlushViewOfFile(target, end - at)) {
        errno = EIO;
        return false;
      }
      at = end;
    }
  }
  return true;
}

// Observe actual protection even after a partially applied mprotect failure.
// Pages losing write access were synchronized first; writable pages can change
// again after this call, so a successful msync alone never marks them clean.
void SharedCopyAccessLocked(uintptr_t lo, uintptr_t hi) {
  for (SharedCopy& copy : SharedCopies()) {
    const uintptr_t a = std::max(lo, copy.start), b = std::min(hi, copy.start + copy.len);
    for (uintptr_t at = a; at < b; at += kPage) {
      MEMORY_BASIC_INFORMATION info;
      if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) continue;
      const DWORD access = info.Protect & 0xff;
      const bool writable = access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
                            access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
      copy.backing->readonly_clean[(copy.backing_offset + at - copy.start) / kPage] = !writable;
    }
  }
}

// Retire only pages that were successfully unmapped/replaced. In particular,
// removing a prefix must not leave a later writeback reading its freed pages.
void ForgetSharedCopiesLocked(uintptr_t lo, uintptr_t hi) {
  auto& copies = SharedCopies();
  for (size_t i = 0; i < copies.size();) {
    SharedCopy& copy = copies[i];
    const uintptr_t end = copy.start + copy.len;
    const uintptr_t a = std::max(lo, copy.start), b = std::min(hi, end);
    if (a >= b) { ++i; continue; }
    if (a == copy.start && b == end) {
      copies.erase(copies.begin() + i);
      continue;
    }
    if (a == copy.start) {
      copy.backing_offset += b - copy.start;
      copy.start = b;
      copy.len = end - b;
    } else if (b == end) {
      copy.len = a - copy.start;
    } else {
      SharedCopy suffix = copy;
      suffix.backing_offset += b - copy.start;
      suffix.start = b;
      suffix.len = end - b;
      copy.len = a - copy.start;
      copies.push_back(std::move(suffix));
    }
    ++i;
  }
}

// A base mapping can fail after removing destination pages. Preserve records
// for untouched pages, but never read a now-free/reserved/replaced page later.
// Plain in-place anonymous replacement needs the mapper's explicit outcome:
// its new allocation may have the same VirtualQuery identity as the old one.
void ReconcileFailedMappingLocked(uintptr_t lo, uintptr_t hi, bool private_replaced) {
  const int saved_errno = errno;
  if (private_replaced) {
    ForgetSharedCopiesLocked(lo, hi);
  } else {
    std::vector<std::pair<uintptr_t, uintptr_t>> retired;
    for (const SharedCopy& copy : SharedCopies()) {
      const uintptr_t a = std::max(lo, copy.start), b = std::min(hi, copy.start + copy.len);
      for (uintptr_t at = a; at < b;) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) break;
        const uintptr_t end = std::min(b,
            reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
        if (end <= at) break;
        if (info.State != MEM_COMMIT || info.Type != MEM_PRIVATE) retired.emplace_back(at, end);
        at = end;
      }
    }
    for (const auto& range : retired) ForgetSharedCopiesLocked(range.first, range.second);
  }
  errno = saved_errno;
}

// A shared file view with a partial final page cannot replace a Windows
// placeholder. Retain a native view elsewhere and copy its mapped pages at
// the requested address, with writeback on synchronization or retirement.
// Publish the returned record only after the old mapping's record is retired.
void* CopyFileMapping(void* addr, size_t len, int prot, int flags, int fd, int64_t offset,
                      SharedCopy* copy) {
  const int keep = dup(fd);
  if (keep < 0) return MAP_FAILED;
  auto backing = std::make_shared<SharedMapping>();
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(keep));
  LARGE_INTEGER file_length;
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const size_t span = (len + kPage - 1) & ~(kPage - 1);
  const uint64_t begin = static_cast<uint64_t>(offset);
  const size_t padding = static_cast<size_t>(begin % info.dwAllocationGranularity);
  auto fail = [&](void* mapped) {
    const int saved_errno = errno;
    if (mapped != nullptr) munmap(mapped, len);
    close(keep);
    errno = saved_errno;
    return MAP_FAILED;
  };
  if (!GetFileSizeEx(file, &file_length)) {
    errno = EBADF;
    return fail(nullptr);
  }
  if (file_length.QuadPart <= 0) { errno = EINVAL; return fail(nullptr); }
  const uint64_t file_size = static_cast<uint64_t>(file_length.QuadPart);
  // Whole pages past EOF would need SIGBUS behavior, not anonymous zeros.
  if (begin >= file_size || span > ((file_size - begin + kPage - 1) & ~(kPage - 1)) ||
      span > SIZE_MAX - padding) {
    errno = EINVAL;
    return fail(nullptr);
  }
  backing->section = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
  backing->writable = backing->section != nullptr;
  if (backing->section == nullptr && (prot & PROT_WRITE) == 0) {
    backing->section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  }
  if (backing->section == nullptr) { errno = EACCES; return fail(nullptr); }
  const uint64_t view_offset = begin - padding;
  // A zero view length includes the physical partial page without extending
  // the section or the file's EOF. Never request a larger section maximum.
  const size_t view_len = span <= file_size - begin ? span + padding : 0;
  backing->view = static_cast<uint8_t*>(MapViewOfFile(backing->section,
      backing->writable ? FILE_MAP_WRITE : FILE_MAP_READ,
      static_cast<DWORD>(view_offset >> 32), static_cast<DWORD>(view_offset), view_len));
  if (backing->view == nullptr) { errno = ENOMEM; return fail(nullptr); }
  backing->data = backing->view + padding;
  backing->readonly_clean.assign(span / kPage, (prot & PROT_WRITE) == 0);
  void* p = mmap(addr, len, PROT_READ | PROT_WRITE,
                 (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    ReconcileFailedMappingLocked(reinterpret_cast<uintptr_t>(addr),
        PageEnd(reinterpret_cast<uintptr_t>(addr), len), gesher_mmap_replaced_private_pages() != 0);
    return fail(nullptr);
  }
  // The previous pages are already gone, even if a later protection step
  // fails. Do not retain their records for a future read from freed memory.
  ForgetSharedCopiesLocked(reinterpret_cast<uintptr_t>(p), PageEnd(reinterpret_cast<uintptr_t>(p), len));
  std::memcpy(p, backing->data, span);
  if (prot != (PROT_READ | PROT_WRITE) && mprotect(p, len, prot) != 0) return fail(p);
  *copy = {reinterpret_cast<uintptr_t>(p), span, 0, std::move(backing)};
  close(keep);  // The retained native section/view now owns the file reference.
  return p;
}

// Code a raw system call maps or makes executable outside every loaded
// object (UPX 5 maps its decompressor from a memfd): an object of its own,
// with no unwind information, rewritten as it runs. Forgotten when unmapped.
LazyObject* NewCodeObjectLocked(uintptr_t lo, uintptr_t hi) {
  auto* o = new LazyObject();
  o->name = "[code mapped by a raw system call]";
  o->lo = lo;
  o->hi = hi;
  o->anonymous = true;
  Objects().push_back(o);
  return o;
}

}  // namespace

int ProtectLoadedMemory(void* addr, size_t len, int prot, bool raw_syscall) {
  const auto start = reinterpret_cast<uintptr_t>(addr);
  if (len == 0 || (start & (kPage - 1)) != 0) return mprotect(addr, len, prot);
  if (len > UINTPTR_MAX - start || start + len > UINTPTR_MAX - (kPage - 1)) {
    errno = EINVAL;
    return -1;
  }
  const uintptr_t end = PageEnd(start, len);
  AcquireSRWLockExclusive(&g_lazy_lock);
  if (!OverlapsObjectLocked(start, end)) {
    if ((!raw_syscall || (prot & PROT_EXEC) == 0) && !OverlapsSharedCopyLocked(start, end)) {
      ReleaseSRWLockExclusive(&g_lazy_lock);
      return mprotect(addr, len, prot);
    }
    if (raw_syscall && (prot & PROT_EXEC) != 0) NewCodeObjectLocked(start, end);
  }
  if ((prot & PROT_WRITE) == 0 && !SharedCopies().empty() && !WriteBackLocked(start, end)) {
    ReleaseSRWLockExclusive(&g_lazy_lock);
    return -1;
  }
  int saved_errno = 0;
  bool ok = ForEachObjectRun(start, end,
      [&](uintptr_t lo, uintptr_t hi, LazyObject* o) {
        const int host = (prot & PROT_EXEC) ? PendingProt(prot) : prot;
        const int result = mprotect(reinterpret_cast<void*>(lo), hi - lo, host);
        const int error = errno;
        SharedCopyAccessLocked(lo, hi);
        if (result != 0) {
          saved_errno = error;
          return false;
        }
        if (prot & PROT_EXEC) DeferPagesLocked(o, lo, hi, prot);
        else ForgetPagesLocked(o, lo, hi);
        return true;
      },
      [&](uintptr_t lo, uintptr_t hi) {
        const int result = mprotect(reinterpret_cast<void*>(lo), hi - lo, prot);
        const int error = errno;
        SharedCopyAccessLocked(lo, hi);
        if (result == 0) return true;
        saved_errno = error;
        return false;
      });
  ReleaseSRWLockExclusive(&g_lazy_lock);
  if (!ok) errno = saved_errno;
  return ok ? 0 : -1;
}

void* MapLoadedMemory(void* addr, size_t len, int prot, int flags, int fd, int64_t offset,
                      bool raw_syscall) {
  const auto hint = reinterpret_cast<uintptr_t>(addr);
  const bool fixed = (flags & MAP_FIXED) != 0;
  const bool enforced = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  const bool replace = fixed && (flags & MAP_FIXED_NOREPLACE) == 0;
  const bool exec = (prot & PROT_EXEC) != 0;
  const int type = flags & 0x0f;
  // Delegate these basic invalid requests before synchronizing a copy. The
  // base layer still decides access rights and allocation-time failures.
  if (len == 0 || len > SIZE_MAX - (kPage - 1) ||
      (enforced && ((hint & (kPage - 1)) != 0 ||
                  ((len + kPage - 1) & ~(kPage - 1)) > UINTPTR_MAX - hint)) ||
      offset < 0 || (static_cast<uint64_t>(offset) & (kPage - 1)) != 0 ||
      (type != MAP_SHARED && type != MAP_PRIVATE && type != MAP_SHARED_VALIDATE) ||
      (type == MAP_SHARED_VALIDATE && (flags & MAP_ANONYMOUS) != 0) ||
      ((flags & MAP_ANONYMOUS) == 0 && (fd < 0 || _get_osfhandle(fd) == -1))) {
    return mmap(addr, len, prot, flags, fd, static_cast<off_t>(offset));
  }
  AcquireSRWLockExclusive(&g_lazy_lock);
  // A copy MAP_FIXED replaces: its writes reach the file first.
  if (replace && !SharedCopies().empty() && !WriteBackLocked(hint, PageEnd(hint, len))) {
    ReleaseSRWLockExclusive(&g_lazy_lock);
    return MAP_FAILED;
  }
  const bool into_object = fixed && len != 0 && OverlapsObjectLocked(hint, PageEnd(hint, len));
  if (!into_object && !(raw_syscall && exec && len != 0) &&
      !(replace && OverlapsSharedCopyLocked(hint, PageEnd(hint, len)))) {
    ReleaseSRWLockExclusive(&g_lazy_lock);
    return mmap(addr, len, prot, flags, fd, static_cast<off_t>(offset));
  }
  // Mapped as asked (a file view keeps its right to execute), then code
  // waits non-executable until it runs.
  SharedCopy copied;
  void* p = mmap(addr, len, prot, flags, fd, static_cast<off_t>(offset));
  if (p == MAP_FAILED && replace) {
    ReconcileFailedMappingLocked(hint, PageEnd(hint, len), gesher_mmap_replaced_private_pages() != 0);
  }
  if (p == MAP_FAILED && errno == EINVAL && replace && (flags & MAP_ANONYMOUS) == 0 &&
      (flags & (MAP_SHARED | MAP_PRIVATE)) != MAP_PRIVATE) {
    p = CopyFileMapping(addr, len, prot, flags, fd, offset, &copied);
  }
  if (p != MAP_FAILED) {
    const auto start = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = PageEnd(start, len);
    if (replace) ForgetSharedCopiesLocked(start, end);
    if (copied.backing) SharedCopies().push_back(std::move(copied));
    if (!OverlapsObjectLocked(start, end)) NewCodeObjectLocked(start, end);
    int protection_error = 0;
    const bool protected_code = ForEachObjectRun(start, end,
        [&](uintptr_t lo, uintptr_t hi, LazyObject* o) {
          if (exec) {
            if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PendingProt(prot)) != 0) {
              protection_error = errno;
              return false;
            }
            DeferPagesLocked(o, lo, hi, prot);
          } else {
            ForgetPagesLocked(o, lo, hi);
          }
          return true;
        },
        [](uintptr_t, uintptr_t) { return true; });  // outside: as asked
    if (!protected_code) {
      // Mapping succeeded but making the code wait for rewriting failed.
      // Never return success with executable, unrewritten pages. Retire the
      // newly mapped range and any partial pending/copy records with it.
      if (munmap(p, len) == 0) {
        ForgetSharedCopiesLocked(start, end);
        ForEachObjectRun(start, end,
            [](uintptr_t lo, uintptr_t hi, LazyObject* o) {
              ForgetPagesLocked(o, lo, hi);
              return true;
            }, [](uintptr_t, uintptr_t) { return true; });
        auto& objects = Objects();
        objects.erase(std::remove_if(objects.begin(), objects.end(),
            [&](const LazyObject* o) { return o->anonymous && o->lo >= start && o->hi <= end; }),
            objects.end());
      }
      errno = protection_error;
      p = MAP_FAILED;
    }
  }
  ReleaseSRWLockExclusive(&g_lazy_lock);
  return p;
}

int SyncLoadedMemory(void* addr, size_t len, int flags) {
  const auto start = reinterpret_cast<uintptr_t>(addr);
  // Invalid flags, alignment or unmapped pages must not publish copy changes.
  // The base call also validates the page-rounded range and its overflow.
  if (msync(addr, len, flags) != 0) return -1;
  const size_t span = (len + kPage - 1) & ~(kPage - 1);
  AcquireSRWLockExclusive(&g_lazy_lock);
  const bool ok = span == 0 || SharedCopies().empty() || WriteBackLocked(start, start + span);
  ReleaseSRWLockExclusive(&g_lazy_lock);
  return ok ? 0 : -1;
}

int UnmapLoadedMemory(void* addr, size_t len) {
  const auto start = reinterpret_cast<uintptr_t>(addr);
  if (len == 0 || (start & (kPage - 1)) != 0) return munmap(addr, len);
  if (len > UINTPTR_MAX - start || start + len > UINTPTR_MAX - (kPage - 1)) {
    errno = EINVAL;
    return -1;
  }
  const uintptr_t end = PageEnd(start, len);
  AcquireSRWLockExclusive(&g_lazy_lock);
  if (!SharedCopies().empty() && !WriteBackLocked(start, end)) {
    ReleaseSRWLockExclusive(&g_lazy_lock);
    return -1;
  }
  const int r = munmap(addr, len);
  const int saved_errno = errno;
  if (r == 0) {
    ForgetSharedCopiesLocked(start, end);
    if (OverlapsObjectLocked(start, end)) {
      ForEachObjectRun(start, end,
          [&](uintptr_t lo, uintptr_t hi, LazyObject* o) {
            ForgetPagesLocked(o, lo, hi);
            return true;
          },
          [](uintptr_t, uintptr_t) { return true; });
      // Code objects of raw system calls go with their whole mapping.
      auto& objects = Objects();
      objects.erase(std::remove_if(objects.begin(), objects.end(),
                                   [&](const LazyObject* o) {
                                     return o->anonymous && o->lo >= start && o->hi <= end;
                                   }),
                    objects.end());
    }
  }
  ReleaseSRWLockExclusive(&g_lazy_lock);
  errno = saved_errno;
  return r;
}

}  // namespace gesher::elf
