/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
/* Actual NDK callers of pinned bionic's file-action registration contract. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;
static int rejected;

static void reject(int result, const char* name, int fd) {
  int saved = errno;
  if (result != EBADF || saved != E2BIG) {
    printf("FAIL  exec4 actions: %s(%d) returned %d, errno %d (expected EBADF, unchanged E2BIG)\n",
           name, fd, result, saved);
    ++failed;
  } else {
    ++rejected;
  }
}

static void check(int okay, const char* name) {
  printf("%s  exec4 actions: %s\n", okay ? "PASS" : "FAIL", name);
  if (!okay) ++failed;
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--child") == 0) return 42;
  if (argc != 2) {
    fprintf(stderr, "usage: exec4-actions <actual-self-path>\n");
    return 2;
  }
#if defined(__aarch64__)
  puts("exec4 actions ABI: arm64-v8a");
#elif defined(__x86_64__)
  puts("exec4 actions ABI: x86_64");
#else
#error Unsupported exec4 fixture ABI
#endif
  posix_spawn_file_actions_t actions;
  int initialized = posix_spawn_file_actions_init(&actions);
  check(initialized == 0, "initialize an actual opaque NDK action list");
  if (initialized != 0) return 1;
  const int invalid[] = {-1, -7, INT_MIN};
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    int fd = invalid[i];
    errno = E2BIG;
    reject(posix_spawn_file_actions_addopen(&actions, fd, "unused-invalid-open", O_RDONLY, 0),
           "addopen", fd);
    errno = E2BIG;
    reject(posix_spawn_file_actions_addclose(&actions, fd), "addclose", fd);
    errno = E2BIG;
    reject(posix_spawn_file_actions_adddup2(&actions, fd, 1), "adddup2 source", fd);
    errno = E2BIG;
    reject(posix_spawn_file_actions_adddup2(&actions, 1, fd), "adddup2 destination", fd);
    errno = E2BIG;
    reject(posix_spawn_file_actions_addfchdir_np(&actions, fd), "addfchdir_np", fd);
  }
  check(rejected == 15, "15 negative descriptor registrations return EBADF without changing errno");

  /* Rejections must leave a usable list. The real children run through the
   * corresponding ELF loader, and their Linux wait status must remain 42. */
  if (failed == 0) {
    for (int search = 0; search < 2; ++search) {
      pid_t pid = -1;
      char* child[] = {argv[1], "--child", NULL};
      int result = search ? posix_spawnp(&pid, argv[1], &actions, NULL, child, environ)
                          : posix_spawn(&pid, argv[1], &actions, NULL, child, environ);
      int status = 0;
      check(result == 0 && pid > 0 && waitpid(pid, &status, 0) == pid &&
                WIFEXITED(status) && WEXITSTATUS(status) == 42,
            search ? "spawnp after rejected actions runs a real child and waits for exit42"
                   : "spawn after rejected actions runs a real child and waits for exit42");
    }
  }
  check(posix_spawn_file_actions_destroy(&actions) == 0, "destroy the rejected-action list");

  /* A positive, unopened fd is accepted at registration time in bionic;
   * execution-time errors belong to the separate child-action contract. */
  initialized = posix_spawn_file_actions_init(&actions);
  check(initialized == 0, "initialize positive descriptor controls");
  if (initialized == 0) {
    const int unopened = INT_MAX;
    check(posix_spawn_file_actions_addopen(&actions, unopened, "unused-positive-open", O_RDONLY, 0) == 0 &&
              posix_spawn_file_actions_addclose(&actions, unopened) == 0 &&
              posix_spawn_file_actions_adddup2(&actions, unopened, unopened) == 0 &&
              posix_spawn_file_actions_addfchdir_np(&actions, unopened) == 0,
          "positive unopened descriptors are accepted without testing parent fd availability");
    check(posix_spawn_file_actions_destroy(&actions) == 0, "destroy positive controls without executing them");
  }
  printf("%d test(s) failed\n", failed);
  return failed == 0 ? 0 : 1;
}
