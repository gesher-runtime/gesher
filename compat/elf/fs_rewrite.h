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
// Code is found by decoding instruction by instruction over the object's FDE
// ranges (.eh_frame_hdr), so bytes inside other instructions or data are not
// mistaken for %fs prefixes; objects without unwind tables are swept over their
// executable sections. See docs/decisions/0014-native-libs-fs-base.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace gesher::elf {

struct FsRewriteStats {
  size_t stack_guard = 0;   // %fs:0x28 -> %gs:0x30
  size_t thread_ptr = 0;    // %fs:0 -> %gs:slot
  size_t stubbed = 0;       // register-based, replayed in a stub
  size_t trapped = 0;       // cannot be rewritten: int3 + diagnostic
  size_t bytes_decoded = 0;
  size_t decode_errors = 0; // bytes skipped because they did not decode
  bool used_fdes = false;   // regions came from .eh_frame_hdr
};

// Rewrites the %fs accesses in a loaded object's executable segments. `file`
// is the object's file image (for program and section headers); the segments
// at `load_bias` must still be writable. Returns false (and sets *error) only
// on a hard failure (no TLS slot, out of memory).
// executable_tls_size is nonzero only for an already-assigned compatible
// main executable, whose linker-resolved negative offsets occupy the first
// static TLS block. Libraries retain their existing rewrite rules.
bool RewriteFsAccesses(const char* name, const uint8_t* file, size_t file_size,
                       uint8_t* load_bias, FsRewriteStats* stats, std::string* error,
                       size_t executable_tls_size = 0);

}  // namespace gesher::elf
