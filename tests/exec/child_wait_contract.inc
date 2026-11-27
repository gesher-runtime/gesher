// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Public Bionic process/thread entries. The shared exec2 driver supplies the
// real child program and assertion reporter; no watcher or clock is replaced.
#include <pthread.h>
#include <stdatomic.h>

struct child_wait_result {
  pid_t child, result;
  int status, error;
  atomic_int entered, done;
};
static void* collect_shared_child(void* value) {
  struct child_wait_result* result = value;
  errno = 0;
  atomic_store(&result->entered, 1);
  result->result = waitpid(result->child, &result->status, 0);
  result->error = errno;
  atomic_store(&result->done, 1);
  return NULL;
}

static atomic_int observed_wait_signals;
static void observe_wait_signal(int sig) {
  (void)sig;
  atomic_fetch_add(&observed_wait_signals, 1);
}

static int await_count(atomic_int* counter, int count) {
  for (int attempt = 0; attempt < 300; ++attempt) {
    if (atomic_load(counter) >= count) return 1;
    usleep(10000);
  }
  return 0;
}

static void child_wait_notification_contract(const char* child) {
  int output[2] = {-1, -1};
  pid_t pid = -1;
  int spawned = -1, ready = 0;
  if (pipe(output) == 0) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);
    char* arguments[] = {(char*)child, (char*)"sigloop", NULL};
    spawned = posix_spawn(&pid, child, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output[1]);
    FILE* stream = fdopen(output[0], "r");
    char line[64] = {0};
    if (stream) {
      ready = spawned == 0 && fgets(line, sizeof(line), stream) != NULL &&
              strcmp(line, "child-ready\n") == 0;
      fclose(stream);
    } else {
      close(output[0]);
    }
  }
  ok("wait notification fixture observes a live child", ready, "native spawn/readiness");
  if (spawned == 0) {
    int status = 0x2468;
    ok("WNOHANG retains live-child status", waitpid(pid, &status,
        WNOHANG | WUNTRACED | WCONTINUED) == 0 && status == 0x2468,
        "live child has no eligible exit/stop/continue result");
    struct sigaction observe = {0}, saved;
    observe.sa_handler = observe_wait_signal;
    sigemptyset(&observe.sa_mask);
    atomic_store(&observed_wait_signals, 0);
    int observe_active = sigaction(SIGUSR1, &observe, &saved) == 0;
    struct child_wait_result interrupted = {.child = pid, .status = 0x2468};
    pthread_t interrupt_thread;
    int interrupt_started = observe_active
        && pthread_create(&interrupt_thread, NULL, collect_shared_child, &interrupted) == 0;
    int interrupted_live = interrupt_started && await_count(&interrupted.entered, 1)
        && pthread_kill(interrupt_thread, SIGUSR1) == 0 && await_count(&interrupted.done, 1);
    if (interrupt_started && !interrupted_live) kill(pid, SIGTERM);
    if (interrupt_started) pthread_join(interrupt_thread, NULL);
    ok("non-restarting signal interrupts a live child wait",
        interrupted_live && atomic_load(&observed_wait_signals) == 1
            && interrupted.result == -1 && interrupted.error == EINTR && interrupted.status == 0x2468,
        "waitpid returns EINTR before child exit");

    observe.sa_flags = SA_RESTART;
    int restart_active = observe_active && sigaction(SIGUSR1, &observe, NULL) == 0;
    atomic_store(&observed_wait_signals, 0);
    struct child_wait_result results[2] = {{.child = pid, .status = -1}, {.child = pid, .status = -1}};
    pthread_t threads[2];
    int started[2];
    for (int i = 0; i < 2; ++i) started[i] = pthread_create(&threads[i], NULL, collect_shared_child, &results[i]) == 0;
    // On the Windows x86-64 shim, the handler can run only when the worker is in waitpid.
    // Requiring delivery while the child is still alive makes both contenders exercise waiting.
    int restarted = restart_active && started[0] && started[1]
        && await_count(&results[0].entered, 1) && await_count(&results[1].entered, 1)
        && pthread_kill(threads[0], SIGUSR1) == 0 && pthread_kill(threads[1], SIGUSR1) == 0
        && await_count(&observed_wait_signals, 2)
        && !atomic_load(&results[0].done) && !atomic_load(&results[1].done);
    ok("restarting signals leave both child waits active", restarted, "SA_RESTART while the child is alive");
    int killed = kill(pid, SIGTERM) == 0;
    for (int i = 0; i < 2; ++i) if (started[i]) pthread_join(threads[i], NULL);
    if (!started[0] && !started[1]) waitpid(pid, NULL, 0);
    if (observe_active) sigaction(SIGUSR1, &saved, NULL);
    int collected = 0, exhausted = 0;
    for (int i = 0; i < 2; ++i) {
      if (results[i].result == pid && WIFEXITED(results[i].status) && WEXITSTATUS(results[i].status) == 43) ++collected;
      if (results[i].result == -1 && results[i].error == ECHILD) ++exhausted;
    }
    ok("competing waiters collect one child once and both retire",
        restarted && killed && collected == 1 && exhausted == 1,
        "exit notification and final-removal notification");
    errno = 0;
    ok("reaped child cannot be collected again", waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD,
        "exactly-once public reaping");
  }

  // Fast exits can precede either registration publication or waiter entry.
  // Observe each actual identity/status through the public any-child route.
  pid_t children[4] = {-1, -1, -1, -1};
  int started = 0, collected = 0, valid = 1;
  for (int i = 0; i < 4; ++i) {
    char code[8]; snprintf(code, sizeof(code), "%d", 30 + i);
    char* arguments[] = {(char*)child, (char*)"exit", code, NULL};
    if (posix_spawn(&children[i], child, NULL, NULL, arguments, environ) == 0) ++started;
  }
  while (collected < started) {
    int status = -1;
    pid_t result = waitpid(-1, &status, 0);
    if (result < 0) { valid = 0; break; }
    int index = 0;
    while (index < 4 && children[index] != result) ++index;
    if (index == 4 || !WIFEXITED(status) || WEXITSTATUS(status) != 30 + index) valid = 0;
    if (index < 4) children[index] = -1;
    ++collected;
  }
  ok("any-child wait retains fast-exit identities and statuses", started == 4 && collected == 4 && valid,
      "four independent spawned children");

  struct sigaction ignore = {0}, previous;
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  int auto_reaped = 0;
  if (sigaction(SIGCHLD, &ignore, &previous) == 0) {
    char* arguments[] = {(char*)child, (char*)"exit", (char*)"39", NULL};
    pid_t ignored;
    if (posix_spawn(&ignored, child, NULL, NULL, arguments, environ) == 0) {
      int status = 0x2468;
      errno = 0;
      auto_reaped = waitpid(ignored, &status, 0) == -1 && errno == ECHILD && status == 0x2468;
    }
    auto_reaped &= sigaction(SIGCHLD, &previous, NULL) == 0;
  }
  ok("ignored SIGCHLD wakes a blocking waiter without a zombie", auto_reaped,
      "auto-reap returns ECHILD and preserves status");
}
