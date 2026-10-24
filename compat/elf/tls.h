// gesher ELF loader: thread-local storage for loaded Android objects.
//
// Android x86-64 code reaches thread-locals and the stack-protector cookie
// through %fs (variant II: the thread pointer is the top of the static TLS
// block, thread-locals at negative offsets, bionic's TCB slots at %fs:0, with
// the stack guard at %fs:0x28). Windows keeps no user %fs base, so the loader
// rewrites those accesses to go through %gs (fs_rewrite.h): the thread pointer
// lives in a TlsAlloc slot of the TEB, and the stack guard is NT_TIB.Self.
//
// For every thread that runs loaded code we build a bionic-shaped block: a
// static TLS area reserved below the thread pointer (committed as modules
// need it) and the TCB slots above it. Each PT_TLS module gets a static
// (initial-exec) offset if it fits in the reserve, so R_X86_64_TPOFF64 and
// __tls_get_addr resolve against the same block; a module loaded after
// threads exist is copied into every live thread's block when it is published.
// A module that does not fit is dynamic-only: __tls_get_addr gives each
// thread its own copy on first use, and initial-exec access to it is refused
// at load time. See docs/decisions/0014-native-libs-fs-base.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gesher::elf {

// The PT_TLS of one loaded object.
struct TlsModule {
  bool present = false;
  const void* init_image = nullptr;  // .tdata to copy; .tbss is the rest, zero
  size_t init_size = 0;              // bytes of init_image
  size_t total_size = 0;            // p_memsz: init_size + .tbss
  size_t align = 1;
  size_t module_id = 0;             // 1-based; 0 = unassigned
  int64_t tp_offset = 0;           // offset from the thread pointer (negative)
  bool is_static = false;          // has a tp_offset in every thread's block
  bool ready = false;              // relocated; its image may be copied
};

// Assigns `m` a module id and, if it fits, a static offset from the thread
// pointer (committing the space in every live thread's block). Called once per
// object with PT_TLS, before its relocations.
void TlsAssignModule(TlsModule* m);

// After the object's relocations: copies its TLS image into every live
// thread's block; threads set up later copy it themselves.
void TlsPublishModule(TlsModule* m);

// The thread-pointer offset for R_X86_64_TPOFF64 against `m`.
int64_t TlsTpOffset(const TlsModule& m);

// Builds this thread's TLS block (once) and stores its thread pointer in the
// TEB slot the rewritten code reads. Idempotent. Returns false only if no
// inline TLS slot or memory is available.
bool TlsSetupThread();

// __tls_get_addr: the address of module `module_id`'s variable at `offset` in
// the current thread (setting the thread up if needed). The bionic shim
// exports this to loaded code.
void* TlsGetAddr(size_t module_id, size_t offset);

// The TEB offset (0x1480 + 8 * index) of the TlsAlloc slot holding the thread
// pointer, allocating the slot on first use; 0 if none below 64 is free.
uint32_t TlsThreadPointerSlot();

// The calling thread's thread pointer, or 0 if it has no block yet.
uintptr_t TlsCurrentThreadPointer();

}  // namespace gesher::elf
