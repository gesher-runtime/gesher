// gesher ELF loader: thread-local storage for loaded Android objects.
//
// Android x86-64 code reaches thread-locals and the stack-protector cookie
// through %fs (variant II: the thread pointer is the top of the static TLS
// block, thread-locals at negative offsets, bionic's TCB slots at %fs:0, with
// the stack guard at %fs:0x28). Windows leaves %fs base 0, so for every thread
// that runs loaded code we build a bionic-shaped block and point %fs at it with
// WRFSBASE (see docs/decisions/0014-native-libs-fs-base.md and the fsbase
// probe). Each PT_TLS module is given static (initial-exec) offsets, so both
// R_X86_64_TPOFF64 and __tls_get_addr resolve against the same per-thread
// block.
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
};

// Assigns `m` a module id and a static offset from the thread pointer. Called
// once per object with PT_TLS, under the loader's lock.
void TlsAssignModule(TlsModule* m);

// The thread-pointer offset for R_X86_64_TPOFF64 against `m`.
int64_t TlsTpOffset(const TlsModule& m);

// Builds this thread's TLS block (copying every assigned module's init image)
// and points %fs at it. Idempotent per thread. Returns false if %fs cannot be
// set on this host.
bool TlsSetupThread();

// __tls_get_addr(GET_TLS_DESC): the address of module `module_id`'s variable at
// `offset` in the current thread. The bionic shim exports this to loaded code.
void* TlsGetAddr(size_t module_id, size_t offset);

}  // namespace gesher::elf
