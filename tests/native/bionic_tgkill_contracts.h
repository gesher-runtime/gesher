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
#include <fenv.h>
#include <linux/futex.h>
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

// Blocks in an untimed futex wait, as Chromium's StackCopierSignal and
// WaitableEvent do, retrying on EINTR/EAGAIN until the word is set; counts
// the EINTR returns a handler without SA_RESTART causes.
static int g_tk_futex_word, g_tk_futex_eintr;
static void* TkFutexWaiter(void* arg) {
  (void)arg;
  volatile int marker = 0;
  __atomic_store_n(&g_tk_frame, (uintptr_t)&marker, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  while (__atomic_load_n(&g_tk_futex_word, __ATOMIC_ACQUIRE) == 0) {
    long r = syscall(SYS_futex, &g_tk_futex_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    if (r == -1 && errno == EINTR) __atomic_fetch_add(&g_tk_futex_eintr, 1, __ATOMIC_RELEASE);
  }
  return NULL;
}

// Blocks in pthread_cond_wait until told to go on.
static pthread_mutex_t g_tk_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tk_cond = PTHREAD_COND_INITIALIZER;
static int g_tk_go;
static void* TkCondWaiter(void* arg) {
  (void)arg;
  volatile int marker = 0;
  __atomic_store_n(&g_tk_frame, (uintptr_t)&marker, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  pthread_mutex_lock(&g_tk_mutex);
  while (!g_tk_go) pthread_cond_wait(&g_tk_cond, &g_tk_mutex);
  pthread_mutex_unlock(&g_tk_mutex);
  return NULL;
}

#if defined(__aarch64__)
// A handler that changes the rounding mode, which sigreturn restores.
static void TkRoundingHandler(int sig) {
  fesetround(FE_UPWARD);
  __atomic_store_n(&g_tk_info, 0, __ATOMIC_RELAXED);
  TkRecord(sig);
}
static int g_tk_spin_round = -1;

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
      __atomic_store_n(&g_tk_spin_round, fegetround(), __ATOMIC_RELAXED);
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

// Two threads hand a token back and forth through futex waits and wakes
// while signals keep arriving at one of them: every interrupted wait must
// still see its wake, and every host call in the loop still return.
static int g_tk_token, g_tk_pong_done;
static long g_tk_pong_sum[2];
enum { kTkRounds = 20000 };
static void* TkPong(void* arg) {
  const int me = (int)(intptr_t)arg;
  if (me == 1) __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  const pid_t pid = getpid();
  long sum = 0;
  for (int round = 0; round < kTkRounds; ++round) {
    int token;
    while ((token = __atomic_load_n(&g_tk_token, __ATOMIC_ACQUIRE)) % 2 != me) {
      syscall(SYS_futex, &g_tk_token, FUTEX_WAIT_PRIVATE, token, NULL, NULL, 0);
    }
    sum += getpid() == pid;  // a host call each round
    __atomic_add_fetch(&g_tk_token, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &g_tk_token, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  }
  g_tk_pong_sum[me] = sum;
  __atomic_add_fetch(&g_tk_pong_done, 1, __ATOMIC_RELEASE);
  return (void*)(intptr_t)(sum == kTkRounds);
}

// A 2 s relative futex wait, with the time it took and its result.
static int g_tk_timed_errno;
static long g_tk_timed_ms;
static void* TkTimedWaiter(void* arg) {
  (void)arg;
  volatile int marker = 0;
  __atomic_store_n(&g_tk_frame, (uintptr_t)&marker, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  static int word = 0;
  struct timespec timeout = {2, 0}, start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  long r = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL, 0);
  g_tk_timed_errno = r == -1 ? errno : 0;
  clock_gettime(CLOCK_MONOTONIC, &end);
  g_tk_timed_ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
  return NULL;
}

// Blocks SIGURG, then waits in an untimed futex wait until the word is set,
// counting EINTR returns; unblocks it afterwards.
static void* TkBlockedWaiter(void* arg) {
  (void)arg;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGURG);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  volatile int marker = 0;
  __atomic_store_n(&g_tk_frame, (uintptr_t)&marker, __ATOMIC_RELAXED);
  __atomic_store_n(&g_tk_worker_tid, gettid(), __ATOMIC_RELEASE);
  while (__atomic_load_n(&g_tk_futex_word, __ATOMIC_ACQUIRE) == 0) {
    long r = syscall(SYS_futex, &g_tk_futex_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    if (r == -1 && errno == EINTR) __atomic_fetch_add(&g_tk_futex_eintr, 1, __ATOMIC_RELEASE);
  }
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);  // takes it now
  return NULL;
}

// SIGUSR1 (SA_RESTART) runs on a thread in an untimed futex wait and itself
// waits; SIGUSR2 (no SA_RESTART) interrupts that inner wait. The inner wait
// fails with EINTR; the outer one restarts, as SIGUSR1's SA_RESTART decides.
static int g_tk_inner_word, g_tk_inner_entered, g_tk_inner_errno, g_tk_usr2_runs;
static void TkWaitingHandler(int sig) {
  (void)sig;
  __atomic_store_n(&g_tk_inner_entered, 1, __ATOMIC_RELEASE);
  long r = syscall(SYS_futex, &g_tk_inner_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
  __atomic_store_n(&g_tk_inner_errno, r == -1 ? errno : 0, __ATOMIC_RELEASE);
}
static void TkUsr2Handler(int sig) {
  (void)sig;
  __atomic_fetch_add(&g_tk_usr2_runs, 1, __ATOMIC_RELEASE);
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

  // A thread blocked in an untimed futex wait takes the signal there; with
  // SA_RESTART the wait goes on, without it the wait returns EINTR.
  pthread_t waiter;
  g_tk_worker_tid = 0;
  g_tk_futex_word = 0;
  g_tk_futex_eintr = 0;
  if (pthread_create(&waiter, NULL, TkFutexWaiter, NULL) == 0) {
    while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
    const pid_t blocked = g_tk_worker_tid;
    usleep(50000);  // into the wait
    g_tk_runs = 0;
    TkCheck("futex wait: tgkill succeeds", tgkill(pid, blocked, SIGURG) == 0);
    TkCheck("futex wait: the SA_RESTART handler runs on the waiting thread",
            TkWaitRuns(1) && g_tk_tid == blocked && g_tk_info == 1 && g_tk_context_ok == 1);
    usleep(50000);
    TkCheck("futex wait: SA_RESTART restarts the wait (no EINTR)",
            __atomic_load_n(&g_tk_futex_eintr, __ATOMIC_ACQUIRE) == 0);
    struct sigaction no_restart = info;
    no_restart.sa_flags = SA_SIGINFO;
    sigaction(SIGURG, &no_restart, NULL);
    g_tk_runs = 0;
    TkCheck("futex wait: tgkill without SA_RESTART succeeds", tgkill(pid, blocked, SIGURG) == 0);
    int eintr = 0;
    for (int i = 0; i < 5000 && !(eintr = __atomic_load_n(&g_tk_futex_eintr, __ATOMIC_ACQUIRE)); ++i)
      usleep(1000);
    TkCheck("futex wait: without SA_RESTART the handler runs and the wait returns EINTR",
            g_tk_runs == 1 && g_tk_tid == blocked && eintr == 1);
    sigaction(SIGURG, &info, NULL);
    __atomic_store_n(&g_tk_futex_word, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &g_tk_futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(waiter, NULL);
  } else {
    TkCheck("pthread_create(futex waiter)", 0);
  }

  // A thread blocked in pthread_cond_wait takes it there too.
  g_tk_worker_tid = 0;
  g_tk_go = 0;
  if (pthread_create(&waiter, NULL, TkCondWaiter, NULL) == 0) {
    while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
    const pid_t blocked = g_tk_worker_tid;
    usleep(50000);
    g_tk_runs = 0;
    TkCheck("condition wait: tgkill succeeds", tgkill(pid, blocked, SIGURG) == 0);
    TkCheck("condition wait: the handler runs on the waiting thread",
            TkWaitRuns(1) && g_tk_tid == blocked && g_tk_info == 1 && g_tk_context_ok == 1);
    pthread_mutex_lock(&g_tk_mutex);
    g_tk_go = 1;
    pthread_cond_broadcast(&g_tk_cond);
    pthread_mutex_unlock(&g_tk_mutex);
    TkCheck("condition wait: the waiter then still waits for its predicate",
            pthread_join(waiter, NULL) == 0);
  } else {
    TkCheck("pthread_create(condition waiter)", 0);
  }

  {
    // Ping-pong under a stream of signals (SA_RESTART, as Chromium's).
    sigaction(SIGURG, &info, NULL);
    pthread_t a, b;
    g_tk_token = 0;
    g_tk_pong_done = 0;
    g_tk_worker_tid = 0;
    g_tk_pong_sum[0] = g_tk_pong_sum[1] = 0;
    if (pthread_create(&a, NULL, TkPong, (void*)0) == 0 &&
        pthread_create(&b, NULL, TkPong, (void*)1) == 0) {
      while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
      const pid_t target = g_tk_worker_tid;
      g_tk_runs = 0;
      int sent = 0;
      for (int i = 0; i < 10000 && __atomic_load_n(&g_tk_pong_done, __ATOMIC_ACQUIRE) < 2; ++i) {
        if (tgkill(pid, target, SIGURG) == 0) ++sent;
        usleep(1000);
      }
      const int done = __atomic_load_n(&g_tk_pong_done, __ATOMIC_ACQUIRE);
      TkCheck("ping-pong through futex waits completes under a stream of signals",
              done == 2 && sent > 0);
      if (done == 2) {
        void* ok_a = NULL;
        void* ok_b = NULL;
        pthread_join(a, &ok_a);
        pthread_join(b, &ok_b);
        TkCheck("every host call in the interrupted loops returned its result",
                ok_a == (void*)1 && ok_b == (void*)1 && g_tk_pong_sum[1] == kTkRounds);
        // Standard signals coalesce while pending: at most one run per send.
        TkCheck("the handlers ran on the signalled thread",
                g_tk_runs > 0 && g_tk_runs <= sent && g_tk_tid == target);
      }
    } else {
      TkCheck("pthread_create(ping-pong)", 0);
    }
  }

  {
    // A timed futex wait fails with EINTR after a handler, even SA_RESTART.
    pthread_t timed;
    g_tk_worker_tid = 0;
    g_tk_timed_errno = -1;
    if (pthread_create(&timed, NULL, TkTimedWaiter, NULL) == 0) {
      while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
      usleep(100000);
      g_tk_runs = 0;
      tgkill(pid, g_tk_worker_tid, SIGURG);
      pthread_join(timed, NULL);
      TkCheck("a timed futex wait returns EINTR after an SA_RESTART handler",
              g_tk_runs == 1 && g_tk_timed_errno == EINTR && g_tk_timed_ms < 1500);
    } else {
      TkCheck("pthread_create(timed waiter)", 0);
    }
  }

  {
    // A blocked signal does not interrupt the wait; unblocking delivers it.
    sigaction(SIGURG, &plain, NULL);
    pthread_t blocked_waiter;
    g_tk_worker_tid = 0;
    g_tk_futex_word = 0;
    g_tk_futex_eintr = 0;
    if (pthread_create(&blocked_waiter, NULL, TkBlockedWaiter, NULL) == 0) {
      while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
      const pid_t target = g_tk_worker_tid;
      usleep(50000);
      g_tk_runs = 0;
      tgkill(pid, target, SIGURG);
      usleep(200000);
      TkCheck("a blocked signal neither runs nor interrupts the futex wait",
              g_tk_runs == 0 && __atomic_load_n(&g_tk_futex_eintr, __ATOMIC_ACQUIRE) == 0);
      __atomic_store_n(&g_tk_futex_word, 1, __ATOMIC_RELEASE);
      syscall(SYS_futex, &g_tk_futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
      pthread_join(blocked_waiter, NULL);
      TkCheck("unblocking it then runs the handler on that thread",
              g_tk_runs == 1 && g_tk_tid == target);
    } else {
      TkCheck("pthread_create(blocked waiter)", 0);
    }
    sigaction(SIGURG, &info, NULL);
  }

  {
    struct sigaction waiting, usr2, old1, old2;
    memset(&waiting, 0, sizeof(waiting));
    waiting.sa_handler = TkWaitingHandler;
    waiting.sa_flags = SA_RESTART;
    memset(&usr2, 0, sizeof(usr2));
    usr2.sa_handler = TkUsr2Handler;
    sigaction(SIGUSR1, &waiting, &old1);
    sigaction(SIGUSR2, &usr2, &old2);
    pthread_t outer;
    g_tk_worker_tid = 0;
    g_tk_futex_word = 0;
    g_tk_futex_eintr = 0;
    g_tk_inner_word = 0;
    g_tk_inner_entered = 0;
    g_tk_inner_errno = -1;
    g_tk_usr2_runs = 0;
    if (pthread_create(&outer, NULL, TkFutexWaiter, NULL) == 0) {
      while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
      const pid_t target = g_tk_worker_tid;
      usleep(50000);
      tgkill(pid, target, SIGUSR1);
      for (int i = 0; i < 5000 && !__atomic_load_n(&g_tk_inner_entered, __ATOMIC_ACQUIRE); ++i)
        usleep(1000);
      usleep(50000);
      tgkill(pid, target, SIGUSR2);
      for (int i = 0; i < 5000 && __atomic_load_n(&g_tk_inner_errno, __ATOMIC_ACQUIRE) == -1; ++i)
        usleep(1000);
      usleep(50000);
      TkCheck("nested: the handler's own futex wait fails with EINTR (no SA_RESTART)",
              g_tk_usr2_runs == 1 && __atomic_load_n(&g_tk_inner_errno, __ATOMIC_ACQUIRE) == EINTR);
      TkCheck("nested: the interrupted outer wait restarts (its handler's SA_RESTART)",
              __atomic_load_n(&g_tk_futex_eintr, __ATOMIC_ACQUIRE) == 0);
      __atomic_store_n(&g_tk_futex_word, 1, __ATOMIC_RELEASE);
      syscall(SYS_futex, &g_tk_futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
      pthread_join(outer, NULL);
    } else {
      TkCheck("pthread_create(nested waiter)", 0);
    }
    sigaction(SIGUSR1, &old1, NULL);
    sigaction(SIGUSR2, &old2, NULL);
  }

#if defined(__aarch64__)
  // A thread running guest code with no delivery point: the translator stops
  // it at a block boundary, as the kernel interrupts user code.
  // The rounding mode a handler sets does not leak into the code it
  // interrupted, as sigreturn restores FPCR on Linux.
  struct sigaction rounding;
  memset(&rounding, 0, sizeof(rounding));
  rounding.sa_handler = TkRoundingHandler;
  sigaction(SIGURG, &rounding, NULL);
  fesetround(FE_TONEAREST);
  g_tk_runs = 0;
  TkCheck("a handler's rounding mode does not leak into the calling thread",
          tgkill(pid, gettid(), SIGURG) == 0 && g_tk_runs == 1 && fegetround() == FE_TONEAREST);
  // Pass 0: an SA_SIGINFO handler, whose frame reports the stopped code;
  // pass 1: the rounding handler, whose mode the stopped code must not see.
  for (int pass = 0; pass < 2; ++pass) {
    sigaction(SIGURG, pass == 0 ? &info : &rounding, NULL);
    pthread_t spinner;
    g_tk_runs = 0;
    g_tk_spin_seen = 0;
    g_tk_spin_round = -1;
    g_tk_worker_tid = 0;
    if (pthread_create(&spinner, NULL, TkSpinner, NULL) != 0) {
      TkCheck("pthread_create(spinner)", 0);
      continue;
    }
    while (__atomic_load_n(&g_tk_worker_tid, __ATOMIC_ACQUIRE) == 0) usleep(1000);
    const pid_t busy = g_tk_worker_tid;
    TkCheck("tgkill to a thread running guest code succeeds", tgkill(pid, busy, SIGURG) == 0);
    TkCheck("the handler interrupts that thread's computation",
            TkWaitRuns(1) && g_tk_tid == busy &&
            (pass == 1 || (g_tk_info == 1 && g_tk_context_ok == 1)));
    pthread_join(spinner, NULL);
    TkCheck("the interrupted computation then carries on", g_tk_spin_seen == 1);
    if (pass == 1)
      TkCheck("with its own rounding mode, not the handler's", g_tk_spin_round == FE_TONEAREST);
  }
#endif

  sigaction(SIGURG, &old, NULL);
  if (g_tk_failures == 0) printf("Bionic tgkill contracts: all passed\n");
  else printf("Bionic tgkill contracts: %d failure(s)\n", g_tk_failures);
  return g_tk_failures;
}
#endif
