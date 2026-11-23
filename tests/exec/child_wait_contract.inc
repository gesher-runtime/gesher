// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Public Bionic process/thread entries. The shared exec2 driver supplies the
// real child program and assertion reporter; no watcher or clock is replaced.
#include <pthread.h>

struct child_wait_result {
  pid_t child, result;
  int status, error;
};
static void* collect_shared_child(void* value) {
  struct child_wait_result* result = value;
  errno = 0;
  result->result = waitpid(result->child, &result->status, 0);
  result->error = errno;
  return NULL;
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
    struct child_wait_result results[2] = {{pid, -2, -1, 0}, {pid, -2, -1, 0}};
    pthread_t threads[2];
    int started[2];
    for (int i = 0; i < 2; ++i) started[i] = pthread_create(&threads[i], NULL, collect_shared_child, &results[i]) == 0;
    int killed = kill(pid, SIGTERM) == 0;
    for (int i = 0; i < 2; ++i) if (started[i]) pthread_join(threads[i], NULL);
    if (!started[0] && !started[1]) waitpid(pid, NULL, 0);
    int collected = 0, exhausted = 0;
    for (int i = 0; i < 2; ++i) {
      if (results[i].result == pid && WIFEXITED(results[i].status) && WEXITSTATUS(results[i].status) == 43) ++collected;
      if (results[i].result == -1 && results[i].error == ECHILD) ++exhausted;
    }
    ok("competing waiters collect one child once and both retire",
        started[0] && started[1] && killed && collected == 1 && exhausted == 1,
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
}
