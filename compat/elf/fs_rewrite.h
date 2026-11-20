// gesher ELF loader: load-time rewriting of %fs-relative accesses.
//
// Android x86-64 code reaches bionic's thread block through %fs: the stack
// guard at %fs:0x28 in every stack-protected function, the thread pointer at
// %fs:0 and %fs:(%reg) for initial-exec TLS. Windows does not keep a user %fs
// base across kernel entry (ADR 0014, the fsbase probe), so the loader rewrites
// those instructions to reach per-thread state through %gs (the TEB), which
// Windows does maintain:
//
//   %fs:0x28  -> %gs:0x30            NT_TIB.Self: per-thread, constant, non-zero;
//                                     used as the thread's stack cookie
//   %fs:0     -> %gs:[TlsSlots[i]]    a TlsAlloc slot (i < 64) holding the
//                                     thread pointer of the bionic-shaped block
//
// Both keep the instruction's length: only the segment prefix byte (0x64 ->
// 0x65) and the 4-byte displacement change. A register-based operand
// (%fs:(%rax), initial-exec TLS) cannot be rewritten in place; it is moved to
// an out-of-line stub that adds the thread pointer to the base register
// (flags preserved), runs the instruction without the %fs prefix, undoes the
// addition and jumps back. The site becomes a `jmp rel32` to the stub when the
// instruction is at least 5 bytes long and the stub is within +-2 GiB, and an
// int3 that a vectored handler redirects otherwise. A compatible main
// executable's negative absolute local-exec operands use a scratch-register
// stub, preserving its register, flags and red zone; only offsets in the
// executable's own static TLS block qualify. Other unsupported accesses (a
// %fs displacement, a RIP-relative %fs operand, an instruction the stub cannot
// replay) becomes an int3 that, if reached, prints what and where and aborts;
// the load reports it on stderr.
//
// Code is decoded over FDE ranges and code reached from function symbols and
// direct control-flow edges, including code without an FDE. Gaps can contain
// assembly data tables and are not swept as instructions. Executable sections
// (else segments) remain the fallback when no code metadata is available.
// A faulting 64-bit absolute MOV from fs:0/0x28 in a registered ELF executable
// range can also be recovered at execution time without modifying code/data.
// Other faults, unknown displacements and missing TLS retain their failure path.
// See docs/decisions/0014-native-libs-fs-base.md.
//
// Raw Linux system calls (docs/decisions/0084-native-libs-self-unpacking-code.md).
// A `syscall` instruction in loaded code would enter the Windows kernel with a
// Linux system call number (packers' stubs, anti-tamper code): Windows runs
// whatever NT service has that number and returns an NTSTATUS in %rax
// (UPX's mmap came back as 0xffffffffc0000005 and was used as a pointer). Each
// one the rewrite finds becomes `ud2` (same length: 0F 05 -> 0F 0B), and the
// vectored handler emulates the call through the handler the bionic shim
// registers (SetRawSyscallHandler): %rax = number, %rdi, %rsi, %rdx, %r10,
// %r8, %r9 = arguments, the result (or -errno) in %rax, %rcx and %r11
// clobbered as the kernel does.
//
// Code that is not on disk (self-unpacking objects, code written at run time).
// An object whose unwind information describes code outside its executable
// file bytes, whose .eh_frame_hdr is not in its file bytes, or that UPX
// packed, unpacks its code in its initializer: its file bytes there are
// compressed data, which the rewrite must not touch. Such an object is not
// rewritten at load; its executable pages are kept non-executable instead
// (DeferUnpackedCode). Likewise, pages of any loaded object that loaded code
// makes executable again (mprotect/mmap with PROT_EXEC: unpacked or patched
// code, ProtectLoadedMemory/MapLoadedMemory) stay non-executable. The first
// execution in such a page faults; the handler decodes from the faulting
// address (an actual entry point) along direct control flow, plus the FDE
// starts in that page once its code has been regenerated, rewrites what it
// finds, gives the page the protection that was asked for and resumes.
#include <gesher/elf_loader.h>
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>

namespace gesher::elf {

// Owns the registrations and stubs of a not-yet-committed ELF image. Destroy
// before unmapping that image. Ordinary loaded libraries retain this owner for
// their lifetime; this is not an API for unloading executing code.
class FsRewriteRegistration {
 public:
  explicit FsRewriteRegistration(void* object) : object_(object) {}
  ~FsRewriteRegistration();
  FsRewriteRegistration(const FsRewriteRegistration&) = delete;
  FsRewriteRegistration& operator=(const FsRewriteRegistration&) = delete;

 private:
  void* object_;
};

struct FsRewriteStats {
  size_t stack_guard = 0;   // %fs:0x28 -> %gs:0x30
  size_t thread_ptr = 0;    // %fs:0 -> %gs:slot
  size_t stubbed = 0;       // register-based, replayed in a stub
  size_t trapped = 0;       // cannot be rewritten: int3 + diagnostic
  size_t syscalls = 0;      // raw `syscall` -> ud2, emulated by the handler
  size_t bytes_decoded = 0;
  size_t decode_errors = 0; // bytes skipped because they did not decode
  bool used_fdes = false;   // .eh_frame_hdr supplied code ranges
  bool self_unpacking = false;  // not rewritten at load: deferred to execution
};

// Rewrites the %fs accesses in a loaded object's executable segments. `file`
// is the object's file image (for program and section headers); the segments
// at `load_bias` must be mapped readable. The pages it patches are made
// writable (copy-on-write) here; the loader sets the final protection after.
// Returns false (and sets *error) only on a hard failure (no TLS slot, out of
// memory, a page that cannot be made writable).
// executable_tls_size is nonzero only for an already-assigned compatible
// main executable, whose linker-resolved negative offsets occupy the first
// static TLS block. Libraries retain their existing rewrite rules.
// `map_base`/`map_size` are the object's whole reservation, registered for
// the deferred rewrite of code made executable after the load.
bool RewriteFsAccesses(const char* name, const uint8_t* file, size_t file_size,
                       uint8_t* load_bias, uint8_t* map_base, size_t map_size,
                       FsRewriteStats* stats, std::string* error,
                       size_t executable_tls_size = 0,
                       std::unique_ptr<FsRewriteRegistration>* registration = nullptr);

// For an object RewriteFsAccesses found self-unpacking (stats->self_unpacking):
// after the loader's final mprotect, makes its executable segments readable
// but not executable, so that their first execution is rewritten then. False
// (and *error) if the pages cannot be re-protected.
bool DeferUnpackedCode(const char* name, const uint8_t* file, uint8_t* load_bias,
                       std::string* error);

}  // namespace gesher::elf
