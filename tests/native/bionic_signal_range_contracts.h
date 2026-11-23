// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Android 16 Bionic's public realtime range, via both ordinary imports and
// runtime symbol lookup. Included in the same actual x64/ARM64 libc guest.
#ifndef GESHER_BIONIC_SIGNAL_RANGE_CONTRACTS_H
#define GESHER_BIONIC_SIGNAL_RANGE_CONTRACTS_H
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t BionicObservedMask(const sigset_t* mask) {
  uint64_t bits = 0;
  for (int sig = 1; sig <= 64; ++sig)
    if (sigismember(mask, sig) == 1) bits |= UINT64_C(1) << (sig - 1);
  return bits;
}

// These calls exercise the actual shared provider on both guest ABIs. No
// signal is delivered and no raw kernel mask or fixture-only setter is used.
static int BionicReservedMaskContracts(int high_signal) {
  int failures = 0;
  const int saved_errno = errno;
  sigset_t original, empty, requested, old, observed;
  const uint64_t timer = UINT64_C(1) << 31;
  const uint64_t runtime = UINT64_C(0x1ff) << 32;
  const uint64_t high_bit = UINT64_C(1) << (high_signal - 1);
  const uint64_t public_bits = (UINT64_C(1) << (SIGUSR1 - 1)) | high_bit;
  printf("Bionic reserved mask reference upper signal: %d\n", high_signal);
#define MASK_CHECK(label, expression) do { \
    const int ok = (expression); \
    printf("  reserved mask: %-45s %s\n", label, ok ? "ok" : "FAIL"); \
    if (!ok) ++failures; \
  } while (0)
  if (pthread_sigmask(SIG_BLOCK, NULL, &original) != 0 || sigemptyset(&empty) != 0)
    return 1;
  errno = EDOM;
  const int initial_rc = pthread_sigmask(SIG_SETMASK, &empty, &old);
  const int initial_errno = errno;
  MASK_CHECK("SETMASK returns the previous mask and keeps errno",
             initial_rc == 0 && initial_errno == EDOM &&
             BionicObservedMask(&old) == BionicObservedMask(&original));
  MASK_CHECK("empty SETMASK keeps timer32 blocked",
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == timer);

  requested = empty;
  for (int sig = 32; sig <= 41; ++sig) sigaddset(&requested, sig);
  sigaddset(&requested, SIGUSR1);
  sigaddset(&requested, high_signal);
  const uint64_t requested_bits = BionicObservedMask(&requested);
  MASK_CHECK("BLOCK filters reserved bits and preserves its input",
             pthread_sigmask(SIG_BLOCK, &requested, &old) == 0 &&
             BionicObservedMask(&old) == timer &&
             BionicObservedMask(&requested) == requested_bits);
  MASK_CHECK("public lower/upper bits remain blockable",
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == (timer | public_bits));
  requested = empty;
  sigaddset(&requested, 32);
  MASK_CHECK("UNBLOCK cannot release the timer signal",
             pthread_sigmask(SIG_UNBLOCK, &requested, NULL) == 0 &&
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == (timer | public_bits));
  sigaddset(&requested, SIGUSR1);
  sigaddset(&requested, high_signal);
  MASK_CHECK("UNBLOCK still removes public signals",
             sigprocmask(SIG_UNBLOCK, &requested, NULL) == 0 &&
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == timer);

  sigfillset(&requested);
  // The production guest always requests signal64. A standalone QEMU
  // reference can explicitly select a lower upper-half signal because its
  // linux-user kernel mapping drops64 before any Bionic jump executes.
  // Never discover that limitation dynamically and call it a full pass.
  if (high_signal != 64) sigdelset(&requested, 64);
  const uint64_t all_allowed = UINT64_MAX & ~runtime &
      ~(UINT64_C(1) << (SIGKILL - 1)) & ~(UINT64_C(1) << (SIGSTOP - 1)) &
      (high_signal == 64 ? UINT64_MAX : ~(UINT64_C(1) << 63));
  MASK_CHECK("SETMASK filters runtime and unmaskable signals",
             pthread_sigmask(SIG_SETMASK, &requested, NULL) == 0 &&
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == all_allowed);
  MASK_CHECK("query ignores an invalid how without changing state",
             pthread_sigmask(12345, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == all_allowed);
  old = empty;
  errno = ERANGE;
  const int invalid_rc = pthread_sigmask(12345, &empty, &old);
  const int invalid_errno = errno;
  MASK_CHECK("pthread invalid how returns EINVAL and preserves errno/output",
             invalid_rc == EINVAL && invalid_errno == ERANGE &&
             BionicObservedMask(&old) == 0);
  errno = 0;
  const int invalid_process_rc = sigprocmask(12345, &empty, NULL);
  const int invalid_process_errno = errno;
  MASK_CHECK("sigprocmask invalid how returns minus1 and sets errno",
             invalid_process_rc == -1 && invalid_process_errno == EINVAL);
  MASK_CHECK("invalid requests preserve the current mask",
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == all_allowed);

  requested = empty;
  sigaddset(&requested, high_signal);
  MASK_CHECK("new/old alias reads the request before returning old state",
             pthread_sigmask(SIG_SETMASK, &requested, &requested) == 0 &&
             BionicObservedMask(&requested) == all_allowed &&
             pthread_sigmask(SIG_BLOCK, NULL, &observed) == 0 &&
             BionicObservedMask(&observed) == (timer | high_bit));
  MASK_CHECK("restore original public mask",
             pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
#undef MASK_CHECK
  printf("Bionic reserved mask contracts: %s (%d failures)\n",
         failures ? "FAILED" : "all passed", failures);
  errno = saved_errno;
  return failures;
}

static int BionicSignalRangeContracts(void) {
  int failures = 0;
  const int original_errno = errno;
  printf("Bionic realtime range contracts: begin\n");
#define RANGE_CHECK(label, expression) do { \
    const int ok = (expression); \
    printf("  realtime range: %-45s %s\n", label, ok ? "ok" : "FAIL"); \
    if (!ok) ++failures; \
  } while (0)
  int (*minimum)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "__libc_current_sigrtmin");
  int (*maximum)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "__libc_current_sigrtmax");
  RANGE_CHECK("both public function exports resolve", minimum != NULL && maximum != NULL);
  if (minimum != NULL && maximum != NULL) {
    errno = EDOM;
    const int low = SIGRTMIN;
    const int high = SIGRTMAX;
    const int macro_errno = errno;
    RANGE_CHECK("SIGRTMIN excludes ten Bionic reserved signals", low == 42);
    RANGE_CHECK("SIGRTMAX includes the highest kernel signal", high == 64);
    RANGE_CHECK("public macro calls preserve errno", macro_errno == EDOM);

    errno = ERANGE;
    const int looked_up_low = minimum();
    const int looked_up_high = maximum();
    const int lookup_errno = errno;
    RANGE_CHECK("resolved functions agree with ordinary imports",
                looked_up_low == low && looked_up_high == high);
    RANGE_CHECK("resolved function calls preserve errno", lookup_errno == ERANGE);

    int stable = 1;
    for (int i = 0; i != 32; ++i)
      stable = stable && minimum() == low && maximum() == high;
    RANGE_CHECK("repeated queries keep the same public range", stable);
    sigset_t mask;
    RANGE_CHECK("both public endpoints belong to a signal set",
                sigemptyset(&mask) == 0 && sigaddset(&mask, low) == 0 &&
                sigaddset(&mask, high) == 0 && sigismember(&mask, low) == 1 &&
                sigismember(&mask, high) == 1);
  }
  failures += BionicReservedMaskContracts(64);
#undef RANGE_CHECK
  printf("Bionic realtime range contracts: %s (%d failures)\n",
         failures ? "FAILED" : "all passed", failures);
  errno = original_errno;
  return failures;
}
#endif
