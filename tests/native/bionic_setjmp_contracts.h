// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef GESHER_BIONIC_SETJMP_CONTRACTS_H
#define GESHER_BIONIC_SETJMP_CONTRACTS_H

#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Run the same public-API cases on Android Bionic and the translated guest.
// In particular Bionic setjmp saves the mask, unlike glibc's setjmp.
// An upper-half public RT signal tests the 64-bit sigset_t without selecting
// QEMU linux-user's unavailable guest signal 64. Android's separate raw-mask
// oracle and the CPU fixture can cover that endpoint where it is available.
#define JUMP_HIGH_SIGNAL (SIGRTMIN + 5)
static int JumpMask(int first, int second, int high) {
  sigset_t mask;
  if (sigemptyset(&mask) != 0) return -1;
  if (first && sigaddset(&mask, SIGUSR1) != 0) return -1;
  if (second && sigaddset(&mask, SIGUSR2) != 0) return -1;
  if (high && sigaddset(&mask, JUMP_HIGH_SIGNAL) != 0) return -1;
  return sigprocmask(SIG_SETMASK, &mask, NULL);
}

static int JumpMaskIs(int first, int second, int high) {
  sigset_t mask;
  return sigprocmask(SIG_SETMASK, NULL, &mask) == 0 &&
         sigismember(&mask, SIGUSR1) == first &&
         sigismember(&mask, SIGUSR2) == second &&
         sigismember(&mask, JUMP_HIGH_SIGNAL) == high;
}

static void JumpReport(int* failures, const char* label, int ok) {
  if (!ok) {
    printf("FAIL  Bionic setjmp: %s\n", label);
    ++*failures;
  }
}

static void __attribute__((noreturn, noinline)) JumpAlias(sigjmp_buf env, int value, int alias) {
  if (alias == 0) longjmp(env, value);
  if (alias == 1) _longjmp(env, value);
  siglongjmp(env, value);
}

static int JumpMaskCase(int save, int value, int alias) {
  int failures = 0;
  sigset_t original;
  if (sigprocmask(SIG_SETMASK, NULL, &original) != 0 || JumpMask(1, 0, 1) != 0) return 1;
  sigjmp_buf env;
  volatile int phase = 0;
  int result = sigsetjmp(env, save);
  if (result == 0) {
    phase = 1;
    if (JumpMask(0, 1, 0) != 0) return 1;
    JumpAlias(env, value, alias);
  }
  JumpReport(&failures, "sigsetjmp return and preserved volatile local",
             result == (value == 0 ? 1 : value) && phase == 1);
  JumpReport(&failures, "sigsetjmp optional 64-bit mask restoration",
             save ? JumpMaskIs(1, 0, 1) : JumpMaskIs(0, 1, 0));
  JumpReport(&failures, "restore caller mask", sigprocmask(SIG_SETMASK, &original, NULL) == 0);
  return failures;
}

static int JumpNamedCase(int save, int alias) {
  int failures = 0;
  sigset_t original;
  if (sigprocmask(SIG_SETMASK, NULL, &original) != 0 || JumpMask(1, 0, 1) != 0) return 1;
  jmp_buf env;
  volatile int result = 0;
  if (save) result = setjmp(env);
  else result = _setjmp(env);
  if (result == 0) {
    if (JumpMask(0, 1, 0) != 0) return 1;
    JumpAlias(env, -7, alias);
  }
  JumpReport(&failures, "setjmp/_setjmp return value", result == -7);
  JumpReport(&failures, "setjmp saves, _setjmp leaves mask unchanged",
             save ? JumpMaskIs(1, 0, 1) : JumpMaskIs(0, 1, 0));
  JumpReport(&failures, "restore named-case caller mask", sigprocmask(SIG_SETMASK, &original, NULL) == 0);
  return failures;
}

static int JumpNestedAndReuse(void) {
  int failures = 0;
  sigset_t original;
  if (sigprocmask(SIG_SETMASK, NULL, &original) != 0 || JumpMask(1, 0, 1) != 0) return 1;
  sigjmp_buf outer, inner;
  volatile int inner_ok = 0;
  int a = sigsetjmp(outer, 1);
  if (a == 0) {
    if (JumpMask(0, 1, 0) != 0) return 1;
    int b = sigsetjmp(inner, 1);
    if (b == 0) {
      if (JumpMask(1, 1, 1) != 0) return 1;
      siglongjmp(inner, 2);
    }
    inner_ok = b == 2 && JumpMaskIs(0, 1, 0);
    siglongjmp(outer, 3);
  }
  JumpReport(&failures, "nested saved buffers retain independent masks",
             a == 3 && inner_ok && JumpMaskIs(1, 0, 1));

  jmp_buf reused;
  if (setjmp(reused) != 0) return failures + 1;
  if (JumpMask(0, 1, 0) != 0) return failures + 1;
  int c = _setjmp(reused);
  if (c == 0) {
    if (JumpMask(1, 1, 0) != 0) return failures + 1;
    _longjmp(reused, 9);
  }
  JumpReport(&failures, "reusing a saved buffer for _setjmp clears its old mask flag",
             c == 9 && JumpMaskIs(1, 1, 0));
  JumpReport(&failures, "restore nested-case caller mask", sigprocmask(SIG_SETMASK, &original, NULL) == 0);
  return failures;
}

static volatile sig_atomic_t jump_signal_seen;
static void JumpHandler(int signal_number) {
  if (signal_number == SIGUSR1) ++jump_signal_seen;
}

static int JumpPendingSignal(void) {
  int failures = 0;
  sigset_t original;
  struct sigaction action, previous;
  memset(&action, 0, sizeof action);
  action.sa_handler = JumpHandler;
  sigemptyset(&action.sa_mask);
  if (sigprocmask(SIG_SETMASK, NULL, &original) != 0 ||
      sigaction(SIGUSR1, &action, &previous) != 0 || JumpMask(0, 0, 0) != 0) return 1;
  jump_signal_seen = 0;
  sigjmp_buf env;
  volatile int blocked_delivery = 0;
  int result = sigsetjmp(env, 1);
  if (result == 0) {
    if (JumpMask(1, 0, 0) != 0 || raise(SIGUSR1) != 0) return 1;
    blocked_delivery = jump_signal_seen == 0;
    siglongjmp(env, 5);
  }
  JumpReport(&failures, "restoring an unblocked mask delivers a pending returning handler",
             result == 5 && blocked_delivery && jump_signal_seen == 1 && JumpMaskIs(0, 0, 0));
  JumpReport(&failures, "restore pending-case caller mask", sigprocmask(SIG_SETMASK, &original, NULL) == 0);
  JumpReport(&failures, "restore prior signal handler", sigaction(SIGUSR1, &previous, NULL) == 0);
  return failures;
}

static void* JumpThread(void* argument) {
  int failures = 0;
  int index = (int)(intptr_t)argument;
  for (int i = 0; i < 12; ++i) failures += JumpMaskCase((i + index) % 2, i - 6, i % 3);
  return (void*)(intptr_t)failures;
}

static int BionicSetjmpContracts(void) {
  int failures = 0;
  const int saves[] = {0, 1};
  const int values[] = {0, 1, 7, -7, INT_MIN, INT_MAX};
  for (unsigned s = 0; s < sizeof saves / sizeof saves[0]; ++s)
    for (unsigned v = 0; v < sizeof values / sizeof values[0]; ++v)
      for (int alias = 0; alias < 3; ++alias)
        failures += JumpMaskCase(saves[s], values[v], alias);
  for (int alias = 0; alias < 3; ++alias)
    for (int save = 0; save < 2; ++save) failures += JumpNamedCase(save, alias);
  failures += JumpNestedAndReuse();
  failures += JumpPendingSignal();
  pthread_t threads[4];
  int started = 0;
  for (; started < 4; ++started)
    if (pthread_create(&threads[started], NULL, JumpThread, (void*)(intptr_t)started) != 0) break;
  JumpReport(&failures, "start independent mask workers", started == 4);
  for (int i = 0; i < started; ++i) {
    void* result = NULL;
    int status = pthread_join(threads[i], &result);
    JumpReport(&failures, "thread-local jump masks and buffer lifetime", status == 0 && result == NULL);
  }
  printf("Bionic setjmp contracts: %s (%d failures; 90 mask cases, nested/reuse/pending/thread checks)\n",
         failures == 0 ? "all passed" : "FAILED", failures);
  return failures;
}

#endif
