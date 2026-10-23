// gesher bionic shim: what the shim's files share.
#pragma once

#include "abi_types.h"

#include <stdint.h>

#include <cstring>

#include <string>
#include <unordered_map>

namespace gesher::bionic {

// --- the System V va_list -------------------------------------------------

struct SysvVaList {
  uint32_t gp_offset;  // 0..48: next general-purpose register in the save area
  uint32_t fp_offset;  // 48..176: next xmm register in the save area
  uint8_t* overflow_arg_area;
  uint8_t* reg_save_area;
};

inline uint64_t NextGp(SysvVaList* ap) {
  uint64_t v;
  if (ap->gp_offset < 48) {
    std::memcpy(&v, ap->reg_save_area + ap->gp_offset, 8);
    ap->gp_offset += 8;
  } else {
    std::memcpy(&v, ap->overflow_arg_area, 8);
    ap->overflow_arg_area += 8;
  }
  return v;
}

inline double NextFp(SysvVaList* ap) {
  double v;
  if (ap->fp_offset < 176) {
    std::memcpy(&v, ap->reg_save_area + ap->fp_offset, 8);
    ap->fp_offset += 16;
  } else {
    std::memcpy(&v, ap->overflow_arg_area, 8);
    ap->overflow_arg_area += 8;
  }
  return v;
}

// A trampoline for a variadic function with K fixed integer/pointer
// arguments: save rdi..r9 and xmm0-7 as a System V register save area at
// 0(%rsp), build the va_list at 176(%rsp) (gp_offset = 8*K, fp_offset = 48,
// overflow_arg_area = the caller's stack arguments, reg_save_area = %rsp), pass
// its address as argument K+1 (in REG) and call the va_list form. The fixed
// arguments are still in their registers. 216 bytes keep %rsp 16-aligned for
// the call and the movaps.
#define GESHER_VA_TRAMPOLINE(NAME, TARGET, GP_OFFSET, REG) \
  ".globl " NAME "\n"                                      \
  ".p2align 4\n" NAME ":\n"                                \
  "  subq $216, %rsp\n"                                    \
  "  movq %rdi, 0(%rsp)\n"                                 \
  "  movq %rsi, 8(%rsp)\n"                                 \
  "  movq %rdx, 16(%rsp)\n"                                \
  "  movq %rcx, 24(%rsp)\n"                                \
  "  movq %r8, 32(%rsp)\n"                                 \
  "  movq %r9, 40(%rsp)\n"                                 \
  "  movaps %xmm0, 48(%rsp)\n"                             \
  "  movaps %xmm1, 64(%rsp)\n"                             \
  "  movaps %xmm2, 80(%rsp)\n"                             \
  "  movaps %xmm3, 96(%rsp)\n"                             \
  "  movaps %xmm4, 112(%rsp)\n"                            \
  "  movaps %xmm5, 128(%rsp)\n"                            \
  "  movaps %xmm6, 144(%rsp)\n"                            \
  "  movaps %xmm7, 160(%rsp)\n"                            \
  "  movl $" GP_OFFSET ", 176(%rsp)\n"                     \
  "  movl $48, 180(%rsp)\n"                                \
  "  leaq 224(%rsp), %rax\n"                               \
  "  movq %rax, 184(%rsp)\n"                               \
  "  movq %rsp, 192(%rsp)\n"                               \
  "  leaq 176(%rsp), " REG "\n"                            \
  "  call " TARGET "\n"                                    \
  "  addq $216, %rsp\n"                                    \
  "  ret\n"

// Symbol name -> address of the System V entry point (or data object).
using SymbolTable = std::unordered_map<std::string, void*>;

// Each part of the shim adds its symbols.
void RegisterLibc(SymbolTable& table);     // shim.cc
void RegisterStdio(SymbolTable& table);    // stdio_bionic.cc
void RegisterPthread(SymbolTable& table);  // pthread_bionic.cc
void RegisterLocale(SymbolTable& table);   // locale_bionic.cc

// Linux errno values the shim returns where the CRT's numbering differs
// (gesher-base's <errno.h> already uses Linux's; these are for clarity).
constexpr int kEPERM = 1;
constexpr int kESRCH = 3;
constexpr int kEAGAIN = 11;
constexpr int kENOMEM = 12;
constexpr int kEBUSY = 16;
constexpr int kEINVAL = 22;
constexpr int kEDEADLK = 35;
constexpr int kENOSYS = 38;
constexpr int kETIMEDOUT = 110;

// A Linux struct timespec as the .so lays it out (64-bit tv_nsec).
struct LinuxTimespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

// CLOCK_REALTIME now, as a Linux timespec.
LinuxTimespec RealtimeNow();
// Milliseconds from now until `abstime` on CLOCK_REALTIME (or the monotonic
// clock), clamped at 0; for timed waits.
uint32_t MillisUntil(const LinuxTimespec* abstime, bool monotonic);

}  // namespace gesher::bionic
