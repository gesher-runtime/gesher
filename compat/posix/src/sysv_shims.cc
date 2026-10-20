// gesher-base: C library functions callable with the System V ABI.
//
// ART's assembly calls a few C library functions directly, with arguments
// in System V registers; the Windows C library uses the Microsoft ABI.
// See docs/decisions/0006-managed-abi.md.

#include <stdlib.h>
#include <string.h>

extern "C" {

__attribute__((sysv_abi)) void* gesher_sysv_memcpy(void* dst, const void* src, size_t n) {
  return memcpy(dst, src, n);
}

__attribute__((sysv_abi)) void gesher_sysv_free(void* p) { free(p); }

}  // extern "C"
