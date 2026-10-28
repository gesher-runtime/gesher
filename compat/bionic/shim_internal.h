// gesher bionic shim: what the shim's files share.
#pragma once

#include "abi_types.h"
#include <gesher/arm64_va_list.h>

#include <stdint.h>
#include <stddef.h>

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
static_assert(sizeof(SysvVaList) == 24);

inline bool IsArm64VaList(const SysvVaList* ap) {
  return (ap->gp_offset & abi::kArm64VaTagMask) == abi::kArm64VaTag;
}

inline uint64_t NextGp(SysvVaList* ap) {
  uint64_t v;
  if (IsArm64VaList(ap)) {
    uint32_t offset = ap->gp_offset & abi::kArm64VaOffsetMask;
    if (offset < abi::kArm64VaGpBytes) {
      const auto* registers = reinterpret_cast<const abi::Arm64VaRegisters*>(ap->reg_save_area);
      std::memcpy(&v, registers->gp_top - (abi::kArm64VaGpBytes - offset), 8);
      ap->gp_offset += 8;
      return v;
    }
    std::memcpy(&v, ap->overflow_arg_area, 8);
    ap->overflow_arg_area += 8;
    return v;
  }
  if (ap->gp_offset < 48) {
    std::memcpy(&v, ap->reg_save_area + ap->gp_offset, 8);
    ap->gp_offset += 8;
  } else {
    std::memcpy(&v, ap->overflow_arg_area, 8);
    ap->overflow_arg_area += 8;
  }
  return v;
}

// Both ordinary double and the existing binary128-to-double formatter use
// this source selection. A 16-byte stack value retains its required alignment.
inline const uint8_t* NextFpArea(SysvVaList* ap, size_t size) {
  if (IsArm64VaList(ap)) {
    uint32_t offset = ap->fp_offset & abi::kArm64VaOffsetMask;
    if (offset < abi::kArm64VaFpBytes) {
      const auto* registers = reinterpret_cast<const abi::Arm64VaRegisters*>(ap->reg_save_area);
      ap->fp_offset += 16;
      return registers->fp_top - (abi::kArm64VaFpBytes - offset);
    }
  } else if (ap->fp_offset < 176) {
    const uint8_t* source = ap->reg_save_area + ap->fp_offset;
    ap->fp_offset += 16;
    return source;
  }
  auto source = reinterpret_cast<uintptr_t>(ap->overflow_arg_area);
  if (size == 16) source = (source + 15) & ~uintptr_t{15};
  ap->overflow_arg_area = reinterpret_cast<uint8_t*>(source + size);
  return reinterpret_cast<const uint8_t*>(source);
}

inline double NextFp(SysvVaList* ap) {
  double v;
  std::memcpy(&v, NextFpArea(ap, 8), 8);
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
void RegisterPosix(SymbolTable& table);    // posix_bionic.cc
void RegisterSocket(SymbolTable& table);   // socket_bionic.cc
void RegisterSignal(SymbolTable& table);   // signal_bionic.cc
void RegisterTime(SymbolTable& table);     // time_bionic.cc
// Last: fork/exec/spawn/wait, and the vfork-style child's view of fds, the
// environment and the working directory over the entries already there.
void RegisterProcess(SymbolTable& table);  // process_bionic.cc

// kill() of another pid: the shim's own children (also those that exited
// before exec), else gesher-base's mailbox (process_bionic.cc).
int KillOtherProcess(int pid, int sig);

// A signal from outside the program's threads: another process (through
// gesher-base's mailbox) or a child's exit (SIGCHLD, `code` CLD_*, `status`
// si_status). `owned`: SIG_DFL is the shim's to act on; else such a signal
// is left to gesher-base (returns false). signal_bionic.cc.
bool DeliverExternalSignal(int sig, int sender_pid, int code, int status, bool owned);

// Sleeps `ms`, or less if a signal this thread does not block arrives (its
// handler has run then); returns true if interrupted, with what was left.
bool InterruptibleSleep(uint32_t ms, uint32_t* left_ms);

// Runs the handlers of signals sent to this thread that it no longer blocks
// (signal_bionic.cc). Called where Linux would deliver them: blocking waits
// and sleeps, and wherever the signal mask is lowered. Cheap when none are
// pending.
void DeliverPendingSignals();

// bionic's pthread_t of the calling thread (pthread_bionic.cc).
uint64_t PthreadSelfId();
int PthreadCpuClock(uint64_t thread, int* clock_id);
int ReadPthreadCpuClock(int clock_id, int64_t* nanos);

// IEEE binary128 (bionic's x86-64 long double) from a double, as 16 bytes.
void DoubleToBinary128(double d, void* out);

// Linux errno values the shim returns where the CRT's numbering differs
// (gesher-base's <errno.h> already uses Linux's; these are for clarity).
constexpr int kEPERM = 1;
constexpr int kENOENT = 2;
constexpr int kEINTR = 4;
constexpr int kEBADF = 9;
constexpr int kEFAULT = 14;
constexpr int kENOTDIR = 20;
constexpr int kERANGE = 34;
constexpr int kEOPNOTSUPP = 95;
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
