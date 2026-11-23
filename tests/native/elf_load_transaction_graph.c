/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 * Original, self-contained ELF graph fixtures. No libc or platform substitute.
 * Compile each role as its own DSO with a native-TLS-capable C compiler. */
#ifndef TX_ROLE
#error TX_ROLE is required
#endif

extern void tx_event(int role);
/* Keep a PT_TLS allocation in every graph node without requiring a TLS access
 * instruction or __tls_get_addr in the portable full-loader host fixture. */
__thread int tx_tls_anchor = TX_ROLE;

#if TX_ROLE == 14
extern void tx_reenter(void);
#endif
__attribute__((constructor)) static void initialized(void) {
  tx_event(TX_ROLE);
#if TX_ROLE == 14
  tx_reenter();
#endif
}

#if TX_ROLE == 1
int tx_newchild(void) { return 10; }
#elif TX_ROLE == 2
int tx_late(void) { return 20; }
#elif TX_ROLE == 3
extern int tx_late(void);
int tx_middle(void) { return tx_late() + 2; }
#elif TX_ROLE == 4
extern int tx_newchild(void), tx_middle(void);
int tx_root(void) { return tx_newchild() + tx_middle(); }
#elif TX_ROLE == 5
int tx_reloc_child(void) { return 5; }
#elif TX_ROLE == 6
extern int tx_reloc_child(void), tx_missing(void);
int tx_reloc_root(void) { return tx_reloc_child() + tx_missing(); }
#elif TX_ROLE == 7
int tx_shared(void) { return 7; }
#elif TX_ROLE == 8
extern int tx_shared(void);
int tx_left(void) { return tx_shared() + 1; }
#elif TX_ROLE == 9
extern int tx_shared(void);
int tx_right(void) { return tx_shared() + 2; }
#elif TX_ROLE == 10
extern int tx_left(void), tx_right(void);
int tx_diamond(void) { return tx_left() + tx_right(); }
#elif TX_ROLE == 11
extern int tx_cycle_b(void);
int tx_cycle_a(void) { return 11; }
int tx_cycle_answer(void) { return tx_cycle_a() + tx_cycle_b(); }
#elif TX_ROLE == 12
extern int tx_cycle_a(void);
int tx_cycle_b(void) { return 12; }
int tx_cycle_back(void) { return tx_cycle_a(); }
#elif TX_ROLE == 13
int tx_reentrant_child(void) { return 13; }
#elif TX_ROLE == 14
int tx_reentrant_root(void) { return 14; }
#elif TX_ROLE == 15
int tx_concurrent_child(void) { return 15; }
#elif TX_ROLE == 16
extern int tx_concurrent_child(void), tx_barrier(void);
int tx_concurrent_root(void) { return tx_concurrent_child() + tx_barrier(); }
#elif TX_ROLE == 17
int tx_protect_child(void) { return 17; }
#elif TX_ROLE == 18
extern int tx_protect_child(void), tx_arm_protection_failure(void);
int tx_protect_root(void) { return tx_protect_child() + tx_arm_protection_failure(); }
#elif TX_ROLE == 19
int tx_concurrent_fail_child(void) { return 19; }
#elif TX_ROLE == 20
extern int tx_concurrent_fail_child(void), tx_barrier_failure(void);
int tx_concurrent_fail_root(void) {
  return tx_concurrent_fail_child() + tx_barrier_failure();
}
#elif TX_ROLE == 21 || TX_ROLE == 22
// A genuine ELF IFUNC, reached by a data relocation before constructors. The
// resolver uses only local code/data, so it needs no unrelocated external GOT.
static int ifunc_calls;
static int ifunc_value(void) { return TX_ROLE; }
// The IFUNC alias names its resolver as metadata rather than a C call.
// NDK Clang needs the explicit retention marker under -Wunused-function.
static __attribute__((used)) int (*select_ifunc(void))(void) { ++ifunc_calls; return ifunc_value; }
int tx_selected(void) __attribute__((ifunc("select_ifunc")));
static int (*selected_pointer)(void) = tx_selected;
int tx_ifunc_result(void) { return selected_pointer() + 100 * ifunc_calls; }
#elif TX_ROLE == 23
__thread int tx_child_tls = 37;
int tx_child_tls_read(void) { return tx_child_tls; }
#elif TX_ROLE == 24
extern __thread int tx_child_tls;
static int observed_tls;
static int tls_implementation(void) { return observed_tls; }
static __attribute__((used)) int (*select_tls_ifunc(void))(void) {
  observed_tls = tx_child_tls;
  tx_child_tls = 93;
  return tls_implementation;
}
int tx_selected_tls(void) __attribute__((ifunc("select_tls_ifunc")));
static int (*tls_dispatch)(void) = tx_selected_tls;
int tx_ifunc_tls_observed(void) { return tls_dispatch(); }
#else
#error Unknown TX_ROLE
#endif
