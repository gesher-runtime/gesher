// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Native test for POSIX real-time signals, non-coalesced FIFO queued delivery,
// signal ordering, standard signal coalescing, sigtimedwait timeout, and sigsuspend.

#include <signal.h>
#include <sys/resource.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

static int failures = 0;
static int assertions = 0;

static void Check(const char* name, bool condition) {
  ++assertions;
  if (!condition) {
    ++failures;
    printf("FAIL: %s\n", name);
  } else {
    printf("PASS: %s\n", name);
  }
}

static void TestRealtimeNonCoalescedFIFO() {
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGRTMIN);
  Check("pthread_sigmask block SIGRTMIN", pthread_sigmask(SIG_BLOCK, &set, &oldset) == 0);

  // Send 3 instances of SIGRTMIN with integer values 10, 20, 30
  union sigval v1, v2, v3;
  v1.sival_int = 10;
  v2.sival_int = 20;
  v3.sival_int = 30;

  Check("sigqueue 1", sigqueue(getpid(), SIGRTMIN, v1) == 0);
  Check("sigqueue 2", sigqueue(getpid(), SIGRTMIN, v2) == 0);
  Check("sigqueue 3", sigqueue(getpid(), SIGRTMIN, v3) == 0);

  // Retrieve in order with sigwaitinfo
  siginfo_t info = {};
  int sig1 = sigwaitinfo(&set, &info);
  Check("retrieve 1 sig == SIGRTMIN", sig1 == SIGRTMIN);
  Check("retrieve 1 value == 10", info.si_value.sival_int == 10);
  Check("retrieve 1 code == SI_QUEUE", info.si_code == SI_QUEUE);

  info = {};
  int sig2 = sigwaitinfo(&set, &info);
  Check("retrieve 2 sig == SIGRTMIN", sig2 == SIGRTMIN);
  Check("retrieve 2 value == 20", info.si_value.sival_int == 20);

  info = {};
  int sig3 = sigwaitinfo(&set, &info);
  Check("retrieve 3 sig == SIGRTMIN", sig3 == SIGRTMIN);
  Check("retrieve 3 value == 30", info.si_value.sival_int == 30);

  // Also test pointer sigval
  int target_val = 12345;
  union sigval vp;
  vp.sival_ptr = &target_val;
  Check("sigqueue pointer", sigqueue(getpid(), SIGRTMIN, vp) == 0);
  info = {};
  int sigp = sigwaitinfo(&set, &info);
  Check("retrieve ptr sig == SIGRTMIN", sigp == SIGRTMIN);
  Check("retrieve ptr value == &target_val", info.si_value.sival_ptr == &target_val);

  // Queue should now be empty: sigtimedwait with 0 timeout returns EAGAIN
  struct timespec zero_timeout = {0, 0};
  int empty_res = sigtimedwait(&set, &info, &zero_timeout);
  Check("queue empty returns EAGAIN", empty_res == -1 && errno == EAGAIN);

  Check("restore mask", pthread_sigmask(SIG_SETMASK, &oldset, nullptr) == 0);
}

static void TestRealtimeOrdering() {
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGRTMIN);
  sigaddset(&set, SIGRTMIN + 1);
  Check("block SIGRTMIN and SIGRTMIN+1", pthread_sigmask(SIG_BLOCK, &set, &oldset) == 0);

  // Queue in mixed order: (SIGRTMIN+1, 100), (SIGRTMIN, 200), (SIGRTMIN+1, 101), (SIGRTMIN, 201)
  union sigval v;
  v.sival_int = 100;
  sigqueue(getpid(), SIGRTMIN + 1, v);
  v.sival_int = 200;
  sigqueue(getpid(), SIGRTMIN, v);
  v.sival_int = 101;
  sigqueue(getpid(), SIGRTMIN + 1, v);
  v.sival_int = 201;
  sigqueue(getpid(), SIGRTMIN, v);

  // Delivery order must be: lower signal number first (SIGRTMIN before SIGRTMIN+1),
  // and FIFO within the same signal number (200 before 201, 100 before 101).
  siginfo_t info = {};
  int r1 = sigwaitinfo(&set, &info);
  Check("order 1 is SIGRTMIN", r1 == SIGRTMIN && info.si_value.sival_int == 200);

  info = {};
  int r2 = sigwaitinfo(&set, &info);
  Check("order 2 is SIGRTMIN", r2 == SIGRTMIN && info.si_value.sival_int == 201);

  info = {};
  int r3 = sigwaitinfo(&set, &info);
  Check("order 3 is SIGRTMIN+1", r3 == SIGRTMIN + 1 && info.si_value.sival_int == 100);

  info = {};
  int r4 = sigwaitinfo(&set, &info);
  Check("order 4 is SIGRTMIN+1", r4 == SIGRTMIN + 1 && info.si_value.sival_int == 101);

  Check("restore mask", pthread_sigmask(SIG_SETMASK, &oldset, nullptr) == 0);
}

static void TestStandardSignalCoalescing() {
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  Check("block SIGUSR1", pthread_sigmask(SIG_BLOCK, &set, &oldset) == 0);

  // Send SIGUSR1 three times
  union sigval v1, v2, v3;
  v1.sival_int = 1;
  v2.sival_int = 2;
  v3.sival_int = 3;
  Check("send 1 SIGUSR1", sigqueue(getpid(), SIGUSR1, v1) == 0);
  Check("send 2 SIGUSR1", sigqueue(getpid(), SIGUSR1, v2) == 0);
  Check("send 3 SIGUSR1", sigqueue(getpid(), SIGUSR1, v3) == 0);

  sigset_t pending;
  sigemptyset(&pending);
  Check("sigpending", sigpending(&pending) == 0);
  Check("SIGUSR1 is pending", sigismember(&pending, SIGUSR1) == 1);

  // First sigwaitinfo takes the coalesced signal
  siginfo_t info = {};
  int sig = sigwaitinfo(&set, &info);
  Check("retrieved coalesced SIGUSR1", sig == SIGUSR1);

  // Subsequent non-blocking wait must find no further signals pending
  struct timespec zero_timeout = {0, 0};
  int extra = sigtimedwait(&set, &info, &zero_timeout);
  Check("no 2nd or 3rd instance of standard signal", extra == -1 && errno == EAGAIN);

  Check("restore mask", pthread_sigmask(SIG_SETMASK, &oldset, nullptr) == 0);
}

static void TestSigtimedwaitTimeout() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR2);

  // 10ms timeout
  struct timespec timeout = {0, 10000000};
  siginfo_t info = {};
  auto start = std::chrono::steady_clock::now();
  int res = sigtimedwait(&set, &info, &timeout);
  auto elapsed = std::chrono::steady_clock::now() - start;

  Check("sigtimedwait timeout returns -1", res == -1);
  Check("sigtimedwait timeout errno is EAGAIN", errno == EAGAIN);
  Check("sigtimedwait waited approximately timeout duration",
        elapsed >= std::chrono::milliseconds(5));
}

static std::atomic<bool> g_handler_called{false};
static std::atomic<int> g_received_signo{0};
static std::atomic<int> g_received_value{0};

static void SigsuspendHandler(int signo, siginfo_t* info, void*) {
  g_handler_called = true;
  g_received_signo = signo;
  if (info != nullptr) {
    g_received_value = info->si_value.sival_int;
  }
}

static void TestSigsuspend() {
  // Install sigaction for SIGUSR1
  struct sigaction act = {}, oldact = {};
  act.sa_sigaction = SigsuspendHandler;
  act.sa_flags = SA_SIGINFO;
  sigemptyset(&act.sa_mask);
  Check("sigaction for SIGUSR1", sigaction(SIGUSR1, &act, &oldact) == 0);

  // Block SIGUSR1 in the current thread
  sigset_t block_set, old_thread_mask;
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGUSR1);
  Check("block SIGUSR1 in thread", pthread_sigmask(SIG_BLOCK, &block_set, &old_thread_mask) == 0);

  // Prepare suspend mask that blocks all signals EXCEPT SIGUSR1
  sigset_t suspend_mask;
  sigfillset(&suspend_mask);
  sigdelset(&suspend_mask, SIGUSR1);

  g_handler_called = false;
  g_received_signo = 0;
  g_received_value = 0;

  // Background thread delivers SIGUSR1 after 50ms
  std::thread sender([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    union sigval v;
    v.sival_int = 456;
    sigqueue(getpid(), SIGUSR1, v);
  });

  // sigsuspend replaces mask with suspend_mask, waits for unmasked signal,
  // runs handler, restores thread mask, and returns -1 with EINTR.
  int ret = sigsuspend(&suspend_mask);
  Check("sigsuspend returns -1", ret == -1);
#if defined(_WIN32)
  Check("unsupported asynchronous context reports ENOSYS", errno == ENOSYS);
  Check("unsupported context does not run the handler", !g_handler_called.load());
  siginfo_t waiting = {};
  const struct timespec zero = {0, 0};
  Check("unsupported delivery preserves SIGUSR1", sigtimedwait(&block_set, &waiting, &zero) == SIGUSR1);
  Check("synchronous wait retains payload value 456", waiting.si_value.sival_int == 456);
#else
  Check("sigsuspend errno is EINTR", errno == EINTR);
  Check("sigsuspend executed handler", g_handler_called.load());
  Check("handler received SIGUSR1", g_received_signo.load() == SIGUSR1);
  Check("handler received payload value 456", g_received_value.load() == 456);
#endif

  sender.join();

  // Verify thread mask was restored to previous mask (which blocked SIGUSR1)
  sigset_t current_mask;
  sigemptyset(&current_mask);
  pthread_sigmask(0, nullptr, &current_mask);
  Check("mask restored after sigsuspend: SIGUSR1 still blocked",
        sigismember(&current_mask, SIGUSR1) == 1);

  // Restore old action and thread mask
  sigaction(SIGUSR1, &oldact, nullptr);
  pthread_sigmask(SIG_SETMASK, &old_thread_mask, nullptr);
}

static void TestSigsuspendRealtime() {
  struct sigaction act = {}, oldact = {};
  act.sa_sigaction = SigsuspendHandler;
  act.sa_flags = SA_SIGINFO;
  sigemptyset(&act.sa_mask);
  Check("sigaction for SIGRTMIN", sigaction(SIGRTMIN, &act, &oldact) == 0);

  sigset_t block_set, old_thread_mask;
  sigemptyset(&block_set);
  sigaddset(&block_set, SIGRTMIN);
  Check("block SIGRTMIN", pthread_sigmask(SIG_BLOCK, &block_set, &old_thread_mask) == 0);

  sigset_t suspend_mask;
  sigfillset(&suspend_mask);
  sigdelset(&suspend_mask, SIGRTMIN);

  g_handler_called = false;
  g_received_signo = 0;
  g_received_value = 0;

  std::thread sender([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    union sigval v;
    v.sival_int = 789;
    sigqueue(getpid(), SIGRTMIN, v);
  });

  int ret = sigsuspend(&suspend_mask);
  Check("sigsuspend RT returns -1", ret == -1);
#if defined(_WIN32)
  Check("unsupported asynchronous RT context reports ENOSYS", errno == ENOSYS);
  Check("unsupported RT context does not run the handler", !g_handler_called.load());
  siginfo_t waiting = {};
  const struct timespec zero = {0, 0};
  Check("unsupported delivery preserves SIGRTMIN", sigtimedwait(&block_set, &waiting, &zero) == SIGRTMIN);
  Check("synchronous wait retains RT payload 789", waiting.si_value.sival_int == 789);
#else
  Check("sigsuspend RT errno is EINTR", errno == EINTR);
  Check("sigsuspend RT executed handler", g_handler_called.load());
  Check("handler received SIGRTMIN", g_received_signo.load() == SIGRTMIN);
  Check("handler received RT payload 789", g_received_value.load() == 789);
#endif

  sender.join();

  sigset_t current_mask;
  sigemptyset(&current_mask);
  pthread_sigmask(0, nullptr, &current_mask);
  Check("mask restored after RT sigsuspend", sigismember(&current_mask, SIGRTMIN) == 1);

  sigaction(SIGRTMIN, &oldact, nullptr);
  pthread_sigmask(SIG_SETMASK, &old_thread_mask, nullptr);
}

static void TestConfiguredQueueLimit() {
  sigset_t set, oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGRTMAX);
  Check("block SIGRTMAX", pthread_sigmask(SIG_BLOCK, &set, &oldset) == 0);
  struct rlimit limit = {};
  Check("read actual RLIMIT_SIGPENDING", getrlimit(RLIMIT_SIGPENDING, &limit) == 0);

  // SIGQUEUE_MAX is not a fixed 32-instance allocation limit on Android.
  // Keep this ordinary test bounded, including its cleanup on a failed send.
  union sigval value = {};
  int queued = 0;
  for (; queued < 40; ++queued) {
    value.sival_int = 1000 + queued;
    if (sigqueue(getpid(), SIGRTMAX, value) != 0) break;
  }
  Check("forty queued values below configured limit", queued == 40);
  bool fifo = true;
  const struct timespec zero = {0, 0};
  for (int i = 0; i < queued; ++i) {
    siginfo_t info = {};
    fifo &= sigtimedwait(&set, &info, &zero) == SIGRTMAX && info.si_value.sival_int == 1000 + i;
  }
  Check("bounded queue preserves every value", fifo);

  // A zero soft limit avoids guessing the number of other processes' queued
  // signals. Restore the original limit even when the tested send fails.
  struct rlimit zero_limit = limit;
  zero_limit.rlim_cur = 0;
  int changed = setrlimit(RLIMIT_SIGPENDING, &zero_limit);
  Check("set zero soft queue limit", changed == 0);
  if (changed == 0) {
    errno = 0;
    int result = sigqueue(getpid(), SIGRTMAX, value);
    Check("RT sigqueue at actual limit fails EAGAIN", result == -1 && errno == EAGAIN);
    Check("RT kill survives queue exhaustion", kill(getpid(), SIGRTMAX) == 0);
    Check("restore queue limit", setrlimit(RLIMIT_SIGPENDING, &limit) == 0);
    siginfo_t info = {};
    Check("RT kill fallback is waitable", sigtimedwait(&set, &info, &zero) == SIGRTMAX);
    Check("fallback does not invent sender or payload", info.si_code == SI_USER &&
          info.si_pid == 0 && info.si_uid == 0 && info.si_value.sival_ptr == nullptr);
  }
  siginfo_t info = {};
  Check("clean queue empty", sigtimedwait(&set, &info, &zero) == -1 && errno == EAGAIN);
  Check("restore mask", pthread_sigmask(SIG_SETMASK, &oldset, nullptr) == 0);
}

#ifdef _WIN32
struct DirectedWaiter {
  sigset_t set{};
  std::atomic<unsigned> phase{0};
  bool inherited = false;
  bool unblocked = false;
  int signal = -1;
  siginfo_t info{};
};

static bool AwaitPhase(const DirectedWaiter& waiter, unsigned phase) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (waiter.phase.load(std::memory_order_acquire) < phase) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

static void* WaitForDirectedQuit(void* argument) {
  auto& waiter = *static_cast<DirectedWaiter*>(argument);
  sigset_t inherited{};
  waiter.inherited = pthread_sigmask(0, nullptr, &inherited) == 0 && sigismember(&inherited, SIGQUIT) == 1;
  if (!AwaitPhase(waiter, 1)) return nullptr;
  const struct timespec timeout = {0, 250000000};
  waiter.signal = sigtimedwait(&waiter.set, &waiter.info, &timeout);
  waiter.unblocked = pthread_sigmask(SIG_UNBLOCK, &waiter.set, nullptr) == 0;
  waiter.phase.store(2, std::memory_order_release);
  AwaitPhase(waiter, 3);
  return nullptr;
}

static void TestDirectedThreadSignal() {
  DirectedWaiter waiter;
  sigset_t original{};
  sigemptyset(&waiter.set);
  sigaddset(&waiter.set, SIGQUIT);
  const int mask = pthread_sigmask(SIG_BLOCK, &waiter.set, &original);
  Check("parent blocks the same shutdown signal as ART", mask == 0);
  if (mask != 0) return;

  pthread_t worker{};
  const int created = pthread_create(&worker, nullptr, WaitForDirectedQuit, &waiter);
  Check("create actual pthread with inherited shutdown mask", created == 0);
  if (created == 0) {
    Check("signal zero probes the live target", pthread_kill(worker, 0) == 0);
    Check("directed SIGQUIT queued before target enters sigwait", pthread_kill(worker, SIGQUIT) == 0);
    const struct timespec immediate = {0, 0};
    Check("an unrelated blocked waiter cannot take directed SIGQUIT",
          sigtimedwait(&waiter.set, nullptr, &immediate) == -1 && errno == EAGAIN);
    waiter.phase.store(1, std::memory_order_release);
    const bool waiting = AwaitPhase(waiter, 2);
    Check("directed waiter returns under its own deadline", waiting);
    if (waiting) {
      Check("other-thread unblocked delivery reports ENOSYS",
            waiter.unblocked && pthread_kill(worker, SIGQUIT) == ENOSYS);
    }
    waiter.phase.store(3, std::memory_order_release);
    Check("join the actual catcher thread", pthread_join(worker, nullptr) == 0);
    Check("catcher inherited a blocked signal without changing its own mask", waiter.inherited);
    Check("only catcher received SIGQUIT with directed sender metadata",
          waiter.signal == SIGQUIT && waiter.info.si_signo == SIGQUIT && waiter.info.si_code == SI_TKILL &&
              waiter.info.si_pid == getpid() && waiter.info.si_uid == getuid());
  }
  Check("restore original thread mask after directed shutdown", pthread_sigmask(SIG_SETMASK, &original, nullptr) == 0);
}
#endif

int main() {
  printf("Running realtime_signals_test...\n");
  TestRealtimeNonCoalescedFIFO();
  TestRealtimeOrdering();
  TestStandardSignalCoalescing();
  TestSigtimedwaitTimeout();
  TestSigsuspend();
  TestSigsuspendRealtime();
  TestConfiguredQueueLimit();
#ifdef _WIN32
  TestDirectedThreadSignal();
#endif

  printf("realtime_signals: %d assertions, %d failures\n", assertions, failures);
  return failures ? 1 : 0;
}
