// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// tgkill(2)/tkill(2) through the actual shim on both guest ABIs: libc calls
// and raw syscall(SYS_tgkill/SYS_tkill) direct a signal at another thread of
// this process by its gettid() ID, and its handler (plain and SA_SIGINFO)
// runs on that thread, as Chromium's stack sampler (SIGURG) requires.
// Included in libc_test.c, which runs as x86-64 and as translated ARM64.
#ifndef GESHER_BIONIC_TGKILL_CONTRACTS_H
#define GESHER_BIONIC_TGKILL_CONTRACTS_H
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static int g_tk_failures;
static void TkCheck(const char* what, int ok) {
  printf("  tgkill: %-62s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++g_tk_failures;
}

// What the last handler saw. Written by the handler, read by the sender.
static int g_tk_sig, g_tk_code, g_tk_pid, g_tk_info, g_tk_context_ok;
static pid_t g_tk_tid;
static int g_tk_runs;
// The receiving thread's own frame and stack, to check the reported context.
static uintptr_t g_tk_frame;
static int g_tk_stop;
#if defined(__aarch64__)
static int g_tk_spin_seen;
#endif
static pid_t g_tk_worker_tid;

static void TkRecord(int sig) {
  __atomic_store_n(&g_tk_sig, sig, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_tid, gettid(), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_tk_runs, 1, __ATOMIC_RELEASE);
}
static void TkPlainHandler(int sig) {
  __atomic_store_n(&g_tk_info, 0, __ATOMIC_RELAXED);
  TkRecord(sig);
}
// The stack pointer the frame reports lies below the receiving thread's own
// live frame, within its stack, and the PC is set: what a stack sampler reads.
static void TkInfoHandler(int sig, siginfo_t* info, void* context) {
  const ucontext_t* uc = (const ucontext_t*)context;
#if defined(__aarch64__)
  uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp, pc = (uintptr_t)uc->uc_mcontext.pc;
#else
  // gregs[REG_RSP] and gregs[REG_RIP] (<sys/ucontext.h> names them only
  // under _GNU_SOURCE).
  uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[15];
  uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[16];
#endif
  uintptr_t frame = __atomic_load_n(&g_tk_frame, __ATOMIC_RELAXED);
  int ok = pc != 0 && sp != 0 && (sp & 7) == 0 &&
           (gettid() != __atomic_load_n(&g_tk_worker_tid, __ATOMIC_RELAXED) ||
            (sp < frame && frame - sp < (8u << 20)));
  __atomic_store_n(&g_tk_context_ok, ok, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_code, info->si_code, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_pid, info->si_pid, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_info, info->si_signo == sig, __ATOMIC_RELAXED);
  TkRecord(sig);
}

// Waits in the shim's sleep (a delivery point) until told to stop.
static void* TkSleeper(void* arg) {
  (void)arg;
  volatile int marker = 0;
  __atomic_store_n(&g_tk_frame, (uintptr_t)&marker, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  while (!__atomic_load_n(&g_tk_stop, __ATOMIC_ACQUIRE)) usleep(1000);
  return NULL;
}

#if defined(__aarch64__)
// Computes without any call into libc: only the translator can stop it, as
// the kernel interrupts user code (clock_gettime is no delivery point). Gives
// up after 10 seconds.
static void* TkSpinner(void* arg) {
  (void)arg;
  volatile int marker = 0;
  __atomic_store_n(&g_tk_frame, (uintptr_t)&marker, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  uint64_t x = 1;
  for (uint64_t round = 1;; ++round) {
    x = x * 6364136223846793005u + 1442695040888963407u;
    if ((round & 0xfffff) != 0) continue;
    if (__atomic_load_n(&g_tk_runs, __ATOMIC_ACQUIRE) != 0) {
      __atomic_store_n(&g_tk_spin_seen, 1, __ATOMIC_RELAXED);
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec > 10) break;
  }
  return (void*)(uintptr_t)(x & 1);
}
#endif

// Waits (bounded) until a handler has run `runs` times in total.
static int TkWaitRuns(int runs) {
  for (int i = 0; i < 5000; ++i) {
    if (__atomic_load_n(&g_tk_runs, __ATOMIC_ACQUIRE) >= runs) return 1;
    usleep(1000);
  }
  return 0;
}

static void* TkExit(void* arg) {
  *(pid_t*)arg = gettid();
  return NULL;
}

static int BionicTgkillContracts(void) {
  printf("Bionic tgkill contracts\n");
  g_tk_failures = 0;
  const pid_t pid = getpid();
  struct sigaction plain, info, old;
  memset(&plain, 0, sizeof(plain));
  plain.sa_handler = TkPlainHandler;
  memset(&info, 0, sizeof(info));
  info.sa_sigaction = TkInfoHandler;
  info.sa_flags = SA_SIGINFO | SA_RESTART;  // as Chromium's StackCopierSignal
  if (sigaction(SIGURG, &plain, &old) != 0) {
    TkCheck("sigaction(SIGURG)", 0);
    return g_tk_failures;
  }

  // The calling thread itself: the handler has run when tgkill returns.
  g_tk_runs = 0;
  TkCheck("tgkill(getpid(), gettid()) runs the handler before returning",
          tgkill(pid, gettid(), SIGURG) == 0 && g_tk_runs == 1 && g_tk_tid == gettid() &&
          g_tk_sig == SIGURG);

  pthread_t sleeper;
  g_tk_stop = 0;
  g_tk_worker_tid = 0;
  if (pthread_create(&sleeper, NULL, TkSleeper, NULL) != 0) {
    TkCheck("pthread_create", 0);
    sigaction(SIGURG, &old, NULL);
    return g_tk_failures;
  }
  while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
  const pid_t worker = g_tk_worker_tid;
  TkCheck("pthread_gettid_np names the same thread", pthread_gettid_np(sleeper) == worker);

  // Existence checks and errors, as the kernel orders them.
  TkCheck("signal 0 checks a live thread (tgkill)", tgkill(pid, worker, 0) == 0);
  TkCheck("signal 0 checks a live thread (tkill)", syscall(SYS_tkill, worker, 0) == 0);
  errno = 0;
  TkCheck("an invalid signal is EINVAL", tgkill(pid, worker, 65) == -1 && errno == EINVAL);
  errno = 0;
  TkCheck("a negative signal is EINVAL", tgkill(pid, worker, -1) == -1 && errno == EINVAL);
  errno = 0;
  TkCheck("tgid 0 is EINVAL", tgkill(0, worker, 0) == -1 && errno == EINVAL);
  errno = 0;
  TkCheck("tid 0 is EINVAL", tgkill(pid, 0, 0) == -1 && errno == EINVAL);
  errno = 0;
  TkCheck("tkill of tid -1 is EINVAL", syscall(SYS_tkill, -1, 0) == -1 && errno == EINVAL);
  errno = 0;
  TkCheck("a thread of this process under another tgid is ESRCH",
          tgkill(pid + 1, worker, 0) == -1 && errno == ESRCH);
  pthread_t gone;
  pid_t gone_tid = 0;
  if (pthread_create(&gone, NULL, TkExit, &gone_tid) == 0 && pthread_join(gone, NULL) == 0) {
    errno = 0;
    TkCheck("an exited thread is ESRCH", tgkill(pid, gone_tid, 0) == -1 && errno == ESRCH);
    errno = 0;
    TkCheck("an exited thread is ESRCH with a signal too",
            tgkill(pid, gone_tid, SIGURG) == -1 && errno == ESRCH);
  } else {
    TkCheck("pthread_create/join of a short thread", 0);
  }

  // A plain handler, by libc tgkill: it runs on the target thread.
  g_tk_runs = 0;
  TkCheck("tgkill(getpid(), other tid, SIGURG) succeeds", tgkill(pid, worker, SIGURG) == 0);
  TkCheck("the plain handler runs on the target thread",
          TkWaitRuns(1) && g_tk_tid == worker && g_tk_sig == SIGURG && g_tk_info == 0);

  // SA_SIGINFO, by libc tgkill, raw syscall(SYS_tgkill) and syscall(SYS_tkill).
  if (sigaction(SIGURG, &info, NULL) != 0) TkCheck("sigaction(SIGURG, SA_SIGINFO)", 0);
  g_tk_runs = 0;
  TkCheck("SA_SIGINFO: tgkill succeeds", tgkill(pid, worker, SIGURG) == 0);
  TkCheck("SA_SIGINFO: the handler runs on the target thread with SI_TKILL",
          TkWaitRuns(1) && g_tk_tid == worker && g_tk_sig == SIGURG && g_tk_info == 1 &&
          g_tk_code == SI_TKILL && g_tk_pid == pid);
  TkCheck("SA_SIGINFO: its ucontext reports the target thread's stack", g_tk_context_ok == 1);
  g_tk_runs = 0;
  TkCheck("raw syscall(SYS_tgkill) to another thread succeeds",
          syscall(SYS_tgkill, pid, worker, SIGURG) == 0);
  TkCheck("raw SYS_tgkill: the handler runs on the target thread",
          TkWaitRuns(1) && g_tk_tid == worker && g_tk_info == 1 && g_tk_context_ok == 1);
  g_tk_runs = 0;
  TkCheck("raw syscall(SYS_tkill) to another thread succeeds",
          syscall(SYS_tkill, worker, SIGURG) == 0);
  TkCheck("raw SYS_tkill: the handler runs on the target thread",
          TkWaitRuns(1) && g_tk_tid == worker && g_tk_info == 1);
  g_tk_runs = 0;
  TkCheck("SA_SIGINFO: tgkill of the calling thread runs it before returning",
          tgkill(pid, gettid(), SIGURG) == 0 && g_tk_runs == 1 && g_tk_tid == gettid() &&
          g_tk_info == 1 && g_tk_code == SI_TKILL && g_tk_context_ok == 1);

  __atomic_store_n(&g_tk_stop, 1, __ATOMIC_RELEASE);
  pthread_join(sleeper, NULL);

#if defined(__aarch64__)
  // A thread running guest code with no delivery point: the translator stops
  // it at a block boundary, as the kernel interrupts user code.
  pthread_t spinner;
  g_tk_runs = 0;
  g_tk_spin_seen = 0;
  g_tk_worker_tid = 0;
  if (pthread_create(&spinner, NULL, TkSpinner, NULL) == 0) {
    while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
    const pid_t busy = g_tk_worker_tid;
    TkCheck("tgkill to a thread running guest code succeeds", tgkill(pid, busy, SIGURG) == 0);
    TkCheck("the handler interrupts that thread's computation",
            TkWaitRuns(1) && g_tk_tid == busy && g_tk_info == 1 && g_tk_context_ok == 1);
    pthread_join(spinner, NULL);
    TkCheck("the interrupted computation then carries on", g_tk_spin_seen == 1);
  } else {
    TkCheck("pthread_create(spinner)", 0);
  }
#endif

  sigaction(SIGURG, &old, NULL);
  if (g_tk_failures == 0) printf("Bionic tgkill contracts: all passed\n");
  else printf("Bionic tgkill contracts: %d failure(s)\n", g_tk_failures);
  return g_tk_failures;
}
#endif
