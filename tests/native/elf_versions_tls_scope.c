// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Ordinary defined TLS references, with deliberately different real offsets.
// Build at -O0 to preserve declaration order; verify the ELF offsets separately.
#if defined(GV_SCOPE_PROVIDER)
__thread int gv_scope_padding[7] = {901, 902, 903, 904, 905, 906, 907};
__thread int gv_scope_tls = 41;
#define GV_SCOPE_OFFSET_FUNCTION gv_scope_provider_ie_offset
#else
#if defined(GV_SCOPE_STATIC)
static __thread int gv_scope_tls __attribute__((used)) = 17;
#elif defined(GV_SCOPE_HIDDEN)
__attribute__((visibility("hidden"))) __thread int gv_scope_tls = 17;
#elif defined(GV_SCOPE_PROTECTED)
__attribute__((visibility("protected"))) __thread int gv_scope_tls = 17;
#else
__thread int gv_scope_tls = 17;
#endif
#if !defined(GV_SCOPE_IE_ONLY)
int gv_scope_tls_read(void) { return gv_scope_tls; }
int* gv_scope_tls_address(void) { return &gv_scope_tls; }
int gv_scope_tls_advance(void) { return gv_scope_tls += 3; }
#endif
#define GV_SCOPE_OFFSET_FUNCTION gv_scope_requester_ie_offset
#endif
// Observe IE relocation values without TP-relative execution on the host.
// Keep IE in a separate DSO: a linker may otherwise relax GD access to IE.
#if defined(GV_SCOPE_PROVIDER) || defined(GV_SCOPE_IE_ONLY)
long GV_SCOPE_OFFSET_FUNCTION(void) {
  long offset;
#if defined(__x86_64__)
  __asm__("movq gv_scope_tls@GOTTPOFF(%%rip), %0" : "=r"(offset));
#elif defined(__aarch64__)
  __asm__("adrp %0, :gottprel:gv_scope_tls\nldr %0, [%0, #:gottprel_lo12:gv_scope_tls]" : "=r"(offset));
#else
#error Unsupported fixture architecture
#endif
  return offset;
}
#endif
