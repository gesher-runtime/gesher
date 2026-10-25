// gesher arm64 translator: internals shared by translator.cc and guest_libc.cc
// (gesher_arm64.dll; docs/arm64.md, docs/decisions/0047-arm64-translation.md).
#pragma once

#include <gesher/arm64_translator.h>

#include <cstddef>
#include <cstdint>

#define GESHER_SYSV __attribute__((sysv_abi))

namespace gesher_arm64 {

// The host table the loader gave gesher_arm64_init.
extern const gesher_arm64_host* g_host;

// A guest trampoline (guest code: `SVC #n; RET`) that calls System V host
// function `fn`; one per function. va_list_index >= 0 marks argument
// `va_list_index` as a va_list to convert from AAPCS64 to System V.
void* BindHost(void* fn, int va_list_index = -2);

// Marks a host function as taking a va_list at `index` (before it is bound).
void AddVaListFunction(const void* fn, int index);

// Places AArch64 instructions in a guest code page (registered as guest code)
// and returns their address: guest-side implementations (setjmp/longjmp).
void* EmitGuestCode(const uint32_t* insns, size_t count);

// Is `addr` inside a registered guest code region?
bool InGuestCode(uintptr_t addr);

// The guest-specific definition of a libc symbol whose host (bionic shim)
// version has an x86-64 ABI the guest cannot use (struct layouts, syscall
// numbers, jmp_buf, auxv): a guest-callable address, or nullptr.
void* GuestSymbol(const char* name);

// Called once by gesher_arm64_init: resolves the host functions guest_libc
// wraps and registers the shim's va_list functions.
void InitGuestLibc();

// A raw `SVC #0` in guest code: Linux arm64 syscall `nr` with x0..x5. Returns
// the raw kernel result (-errno on failure).
int64_t GuestRawSyscall(uint64_t nr, const uint64_t args[6]);

// Linux signal numbers, for the crash report.
constexpr int kSigIll = 4;
constexpr int kSigTrap = 5;
constexpr int kSigSegv = 11;
constexpr int kSigSys = 31;

// Reports a fatal guest fault the way Android's debuggerd does and ends the
// process (never returns).
[[noreturn]] void GuestCrash(int signo, int code, const char* code_name, uint64_t fault_addr,
                             const char* detail);

}  // namespace gesher_arm64
