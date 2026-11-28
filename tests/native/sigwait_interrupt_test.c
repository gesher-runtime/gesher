// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// A thread waiting for SIGUSR2 is interrupted by a handler for SIGUSR1.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifdef GESHER_SIGWAIT_JNI
#include <jni.h>
#endif

enum WaitKind { TIMED, INFO, RETRY, MASKED };
struct Worker {
  enum WaitKind kind;
  atomic_int tid;
  atomic_int finished;
  int setup;
  int result;
  int error;
  int signal;
  int handler_tid;
  int masked_signal;
};

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "the handler uses lock-free stores");
static atomic_int handler_tid;

static void Handler(int signal) {
  if (signal == SIGUSR1) atomic_store_explicit(&handler_tid, gettid(), memory_order_release);
}

static void Delay(void) {
  struct timespec short_wait = {0, 5000000};
  while (nanosleep(&short_wait, &short_wait) == -1 && errno == EINTR) {}
}

static void* Wait(void* argument) {
  struct Worker* worker = argument;
  sigset_t desired, mask;
  sigemptyset(&desired);
  sigaddset(&desired, SIGUSR2);
  mask = desired;
  if (worker->kind == MASKED) sigaddset(&mask, SIGUSR1);
  worker->setup = pthread_sigmask(SIG_BLOCK, &mask, NULL);
  atomic_store_explicit(&worker->tid, gettid(), memory_order_release);
  if (worker->setup == 0) {
    const struct timespec timeout = {0, 450000000};
    siginfo_t info = {0};
    switch (worker->kind) {
      case TIMED:
      case MASKED:
        worker->result = sigtimedwait(&desired, &info, &timeout);
        break;
      case INFO:
        worker->result = sigwaitinfo(&desired, &info);
        break;
      case RETRY:
        worker->result = sigwait(&desired, &worker->signal);
        break;
    }
    worker->error = errno;
    worker->handler_tid = atomic_load_explicit(&handler_tid, memory_order_acquire);
    if (worker->kind == MASKED) {
      const struct timespec zero = {0, 0};
      sigemptyset(&desired);
      sigaddset(&desired, SIGUSR1);
      worker->masked_signal = sigtimedwait(&desired, &info, &zero);
    }
  }
  atomic_store_explicit(&worker->finished, 1, memory_order_release);
  return NULL;
}

static int Check(const char* label, bool pass) {
  printf("sigwait interrupt: %s %s\n", label, pass ? "ok" : "FAIL");
  return pass ? 0 : 1;
}

static int Exercise(enum WaitKind kind) {
  struct Worker worker = {.kind = kind};
  atomic_store_explicit(&handler_tid, 0, memory_order_relaxed);
  pthread_t target;
  if (pthread_create(&target, NULL, Wait, &worker) != 0) return Check("create receiver", false);
  int tid;
  for (int tries = 0; (tid = atomic_load_explicit(&worker.tid, memory_order_acquire)) == 0 &&
       tries < 200; ++tries) Delay();
  // The target publishes its TID immediately before calling the wait.
  for (int i = 0; i < 12; ++i) Delay();
  int sent = kind == TIMED ? syscall(SYS_tgkill, getpid(), tid, SIGUSR1)
                            : pthread_kill(target, SIGUSR1);
  int errors = Check("send an unrelated signal to the receiver", tid > 0 && sent == 0);
  bool completed = false;
  bool handled = false;
  if (kind == INFO || kind == RETRY) {
    for (int i = 0; i < 40; ++i) {
      completed = atomic_load_explicit(&worker.finished, memory_order_acquire) != 0;
      handled = atomic_load_explicit(&handler_tid, memory_order_acquire) == tid;
      if (completed || (kind == RETRY && handled)) break;
      Delay();
    }
    // Safely release even the old implementation which mishandles SIGUSR1.
    if (!completed && pthread_kill(target, SIGUSR2) != 0) {
      errors += Check("release receiver with selected signal", false);
    }
  }
  errors += Check("join receiver", pthread_join(target, NULL) == 0 && worker.setup == 0);
  switch (kind) {
    case TIMED:
      errors += Check("sigtimedwait returns EINTR despite SA_RESTART",
                      worker.result == -1 && worker.error == EINTR && worker.handler_tid == tid);
      break;
    case INFO:
      errors += Check("sigwaitinfo returns EINTR without consuming a selected signal",
                      completed && worker.result == -1 && worker.error == EINTR &&
                      worker.handler_tid == tid);
      break;
    case RETRY:
      errors += Check("sigwait retries after the target handler and takes SIGUSR2",
                      handled && !completed && worker.result == 0 && worker.signal == SIGUSR2 &&
                      worker.handler_tid == tid);
      break;
    case MASKED:
      errors += Check("a blocked unrelated signal stays pending and the wait times out",
                      worker.result == -1 && worker.error == EAGAIN && worker.handler_tid == 0 &&
                      worker.masked_signal == SIGUSR1);
      break;
  }
  return errors;
}

static int RunContract(void) {
  struct sigaction action = {.sa_handler = Handler, .sa_flags = SA_RESTART};
  sigemptyset(&action.sa_mask);
  struct sigaction previous;
  int errors = Check("install receiver handler", sigaction(SIGUSR1, &action, &previous) == 0);
  if (errors == 0) {
    for (int kind = TIMED; kind <= MASKED; ++kind) errors += Exercise((enum WaitKind)kind);
    errors += Check("restore disposition", sigaction(SIGUSR1, &previous, NULL) == 0);
  }
  printf("sigwait interrupt contracts: %s\n", errors == 0 ? "all passed" : "FAILED");
  fflush(stdout);
  return errors;
}

#ifdef GESHER_SIGWAIT_JNI
JNIEXPORT jint JNICALL Java_gesher_SigwaitInterrupt_run(JNIEnv* env, jclass test) {
  (void)env;
  (void)test;
  return RunContract();
}
#else
int main(void) { return RunContract() == 0 ? 0 : 1; }
#endif
