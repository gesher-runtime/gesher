/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * Original valid ELF symbol-version fixtures, linked without a libc. */
#ifndef GV_ROLE
#error GV_ROLE is required
#endif

extern void gv_event(int role);
__attribute__((constructor)) static void initialized(void) { gv_event(GV_ROLE); }

#if GV_ROLE == 1
int gv_value_old(void) { return 11; }
int gv_value_new(void) { return 22; }
__asm__(".symver gv_value_old,gv_value@GV_1");
__asm__(".symver gv_value_new,gv_value@@GV_2");
int gv_data_old = 111;
int gv_data_new = 222;
__asm__(".symver gv_data_old,gv_data@GV_1");
__asm__(".symver gv_data_new,gv_data@@GV_2");

int gv_hidden_old(void) { return 31; }
__asm__(".symver gv_hidden_old,gv_hidden@GV_1");
__attribute__((weak)) int gv_weak_old(void) { return 51; }
__attribute__((weak)) int gv_weak_new(void) { return 52; }
__asm__(".symver gv_weak_old,gv_weak@GV_1");
__asm__(".symver gv_weak_new,gv_weak@@GV_2");
int gv_optional_new(void) { return 82; }
__asm__(".symver gv_optional_new,gv_optional@@GV_2");

/* Different names with equal GNU hashes (bA/ab) or SysV hashes (bA/aQ). */
int gv_bA(void) { return 301; }
int gv_ab(void) { return 302; }
int gv_aQ(void) { return 303; }
/* These version names have equal SysV hashes but different full names. */
int gv_hash_old(void) { return 61; }
int gv_hash_new(void) { return 62; }
__asm__(".symver gv_hash_old,gv_hash_value@GV_bA");
__asm__(".symver gv_hash_new,gv_hash_value@@GV_aQ");

static int ifunc_old_impl(void) { return 71; }
static int ifunc_new_impl(void) { return 72; }
// The resolver is named by the ELF alias, not a C call. Keep it explicitly
// emitted on the supported NDK compiler as well as newer host compilers.
static __attribute__((used)) int (*ifunc_old_resolver(void))(void) { return ifunc_old_impl; }
static __attribute__((used)) int (*ifunc_new_resolver(void))(void) { return ifunc_new_impl; }
int gv_ifunc_old(void) __attribute__((ifunc("ifunc_old_resolver")));
int gv_ifunc_new(void) __attribute__((ifunc("ifunc_new_resolver")));
__asm__(".symver gv_ifunc_old,gv_ifunc@GV_1");
__asm__(".symver gv_ifunc_new,gv_ifunc@@GV_2");
static int null_resolutions;
static __attribute__((used)) int (*null_resolver(void))(void) {
  ++null_resolutions;
  return 0;
}
int gv_null(void) __attribute__((ifunc("null_resolver")));
/* A real shim name detects accidental fallback after successful selection. */
int getpid(void) __attribute__((ifunc("null_resolver")));
int gv_null_resolutions(void) { return null_resolutions; }
#elif GV_ROLE == 2
int gv_value_old(void) { return 11; }
__asm__(".symver gv_value_old,gv_value@@GV_1");
int gv_data_old = 111;
__asm__(".symver gv_data_old,gv_data@@GV_1");
int gv_optional_old(void) { return 81; }
__asm__(".symver gv_optional_old,gv_optional@@GV_1");
#elif GV_ROLE == 3
extern int value_old(void), value_new(void);
extern int data_old, data_new;
__asm__(".symver value_old,gv_value@GV_1");
__asm__(".symver value_new,gv_value@GV_2");
__asm__(".symver data_old,gv_data@GV_1");
__asm__(".symver data_new,gv_data@GV_2");
int gv_client_pair(void) { return 100 * value_old() + value_new(); }
int gv_client_data(void) { return data_old + data_new; }
#elif GV_ROLE == 4
extern int optional(void) __attribute__((weak));
__asm__(".symver optional,gv_optional@GV_2");
int gv_client_weak(void) { return optional ? optional() : -7; }
#elif GV_ROLE == 5
int gv_value(void) { return 33; }
int gv_data = 333;
#elif GV_ROLE == 6
int gv_value(void) { return 44; }
int gv_data = 444;
int gv_tagged(void) { return 66; }
#elif GV_ROLE == 7
__thread int gv_tls_old = 17;
__thread int gv_tls_new = 29;
__asm__(".symver gv_tls_old,gv_tls@GV_1");
__asm__(".symver gv_tls_new,gv_tls@@GV_2");
#elif GV_ROLE == 8
extern __thread int tls_old, tls_new;
__asm__(".symver tls_old,gv_tls@GV_1");
__asm__(".symver tls_new,gv_tls@GV_2");
int gv_tls_pair(void) { return 100 * tls_old + tls_new; }
int gv_tls_advance(void) {
  tls_old += 3;
  tls_new += 5;
  return 100 * tls_old + tls_new;
}
#elif GV_ROLE == 9
__thread int gv_tls_old = 17;
__asm__(".symver gv_tls_old,gv_tls@@GV_1");
#elif GV_ROLE == 10
extern __thread int tls_optional __attribute__((weak));
__asm__(".symver tls_optional,gv_tls@GV_2");
/* The requester's own TLS makes an erroneous self-module fallback visible. */
__thread int gv_tls_anchor = 67;
static int* weak_tls_address(void) {
  int* address = &tls_optional;
  /* Observe the ABI-computed address before an optimizer assumes that a C
   * TLS declaration always has storage. No instruction changes the value. */
  __asm__ volatile("" : "+r"(address));
  return address;
}
int gv_tls_weak_value(void) { int* p = weak_tls_address(); return p ? *p : -7; }
int gv_tls_weak_advance(void) { int* p = weak_tls_address(); return p ? (*p += 5) : -7; }
int gv_tls_anchor_value(void) { return gv_tls_anchor; }
#elif GV_ROLE == 11
extern __thread int tls_optional __attribute__((weak));
extern __thread int tls_required;
__asm__(".weak tls_optional\n.symver tls_optional,gv_tls@GV_2");
__asm__(".symver tls_required,gv_tls@GV_1");
/* Read actual initial-exec relocation slots without dereferencing an absent
 * weak object's TP-relative address. This also runs at the Linux OS boundary. */
#if defined(__x86_64__)
#define GV_IE_OFFSET(symbol) __asm__("movq " #symbol "@GOTTPOFF(%%rip), %0" : "=r"(offset))
#elif defined(__aarch64__)
#define GV_IE_OFFSET(symbol) __asm__("adrp %0, :gottprel:" #symbol "\nldr %0, [%0, #:gottprel_lo12:" #symbol "]" : "=r"(offset))
#else
#error Unsupported fixture architecture
#endif
long gv_tls_ie_optional_offset(void) { long offset; GV_IE_OFFSET(tls_optional); return offset; }
long gv_tls_ie_required_offset(void) { long offset; GV_IE_OFFSET(tls_required); return offset; }
#else
#error Unknown GV_ROLE
#endif
