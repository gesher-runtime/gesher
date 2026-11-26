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

// The exit code of a death by signal `signo` (the shim's
// __gesher_signal_exit_code: a forked child's own code); null without it.
extern unsigned (GESHER_SYSV* g_signal_exit_code)(int);

// A guest trampoline (guest code: `SVC #n; RET`) that calls System V host
// function `fn`; one per function. va_list_index >= 0 marks argument
// `va_list_index` as a va_list to convert from AAPCS64 to System V.
void* BindHost(void* fn, int va_list_index = -2);

// Marks a host function as taking a va_list at `index` (before it is bound).
void AddVaListFunction(const void* fn, int index);

// A variadic function whose variable arguments are all pointers (scanf).
// Call its va_list form without copying a fixed guest-stack window.
void AddPointerVariadicFunction(const void* fn, void* va_fn, int fixed_arguments);

// Places AArch64 instructions in a guest code page (registered as guest code)
// and returns their address: guest-side implementations (setjmp/longjmp).
void* EmitGuestCode(const uint32_t* insns, size_t count);

// Is `addr` inside a registered guest code region?
bool InGuestCode(uintptr_t addr);

// Registers the pages of [start, start + size) as guest code, unless they
// already are: memory the guest made PROT_EXEC itself.
void AddGuestCode(const void* start, size_t size);
// The guest changed the protection of [start, start + size) (mprotect), or
// mapped it (mmap: `mapped`). Making pages executable drops every Jit's
// translations of those that were writable or not executable before (all of
// them for a new mapping, which may reuse an address that held other code).
void GuestProtectionChanged(const void* start, size_t size, bool exec, bool write, bool mapped);
// [start, start + size) was unmapped or mapped by the loader: its protection
// is no longer the guest's last mprotect/mmap of it.
void ForgetGuestProtection(const void* start, size_t size);
// mremap moved [from, from + from_size) to [to, to + to_size): both ranges
// have new contents, so every Jit drops its translations of them; the
// destination is clean guest code only if the whole source was.
void GuestMappingMoved(const void* from, size_t from_size, const void* to, size_t to_size);

// Does [start, start + size) touch the translator's own code pages (the
// trampolines and guest-side helpers it writes)?
bool InTranslatorCode(const void* start, size_t size);

// The guest-specific definition of a libc symbol whose host (bionic shim)
// version has an x86-64 ABI the guest cannot use (struct layouts, syscall
// numbers, jmp_buf, auxv): a guest-callable address, or nullptr.
void* GuestSymbol(const char* name);

// Called once by gesher_arm64_init: resolves the host functions guest_libc
// wraps and registers the shim's va_list functions. False, after a
// diagnostic, when guest code it requires cannot be bound or emitted.
bool InitGuestLibc();

// Memory another process wrote into this one (a ptrace tracer's POKEs, ADR
// 0093), as the host publishes it: each Jit drops what it translated from
// those ranges at its next dispatch.
void SetExternalCodeWrites(const void* writes);
// Makes every running Jit return to its dispatcher (it then drops those
// translations). The host's ptrace guard calls it with every other thread
// suspended: it takes no lock and allocates nothing.
void GESHER_SYSV HaltForExternalWrites();

// The calling guest thread's actual FPCR, including changes made by MSR.
// Host callbacks run outside Jit::Run; setters take effect before resuming.
uint32_t GuestFpcr();
void SetGuestFpcr(uint32_t value);
uint32_t GuestFpsr();
void SetGuestFpsr(uint32_t value);

// A raw `SVC #0` in guest code: Linux arm64 syscall `nr` with x0..x5. Returns
// the raw kernel result (-errno on failure).
int64_t GuestRawSyscall(uint64_t nr, const uint64_t args[6]);

// fork (guest_libc.cc, ADR 0092): the bionic shim calls these with its fork
// set held. TryForkPrepare takes every translator lock or none (1 / 0);
// ForkParent releases them; ForkChild makes them anew in the child.
int GESHER_SYSV TryForkPrepare();
// Releases the Envs of threads that left (their TLS blocks, monitor ids):
// called before a fork, as well as when a thread takes an Env.
void ReleaseRetiredForFork();
void GESHER_SYSV ForkParent();
void GESHER_SYSV ForkChild();

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
