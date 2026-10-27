/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
/* Actual NDK children exercise pinned bionic's ordered spawn actions. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;
static const char data[] = "ordered child bytes\n";
static const char child_name[] = "custom \"argv zero\" \xd7\x92\\";

static void check(int okay, const char* name) {
  printf("%s  exec4 spawn: %s\n", okay ? "PASS" : "FAIL", name);
  if (!okay) ++failed;
}

static int join(char* out, size_t capacity, const char* dir, const char* name) {
  int count = snprintf(out, capacity, "%s/%s", dir, name);
  return count >= 0 && (size_t)count < capacity;
}

static int closed_fd(int fd) {
  errno = 0;
  return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

static int read_data(int fd) {
  char bytes[sizeof(data)] = {0};
  return fd >= 0 && read(fd, bytes, sizeof(data) - 1) == (ssize_t)(sizeof(data) - 1) &&
         memcmp(bytes, data, sizeof(data) - 1) == 0;
}

static int child_main(int argc, char** argv) {
  int okay = 0;
  if (strcmp(argv[1], "--plain") == 0) return 42;
  if (strcmp(argv[1], "--ordered") == 0 && argc == 3) {
    int relative = open("input.bin", O_RDONLY);
    okay = strcmp(argv[0], child_name) == 0 && closed_fd(30) && closed_fd(32) &&
           closed_fd(atoi(argv[2])) && read_data(31) && read_data(relative);
    if (relative >= 0) close(relative);
    char pid[48];
    int length = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    okay = okay && length > 0 && write(33, pid, (size_t)length) == length;
  } else if (strcmp(argv[1], "--closed") == 0 && argc == 3) {
    okay = closed_fd(atoi(argv[2]));
  } else if (strcmp(argv[1], "--keep") == 0 && argc == 4) {
    int fd = atoi(argv[2]), removed = atoi(argv[3]);
    okay = fcntl(fd, F_GETFD) == 0 && read_data(fd) && (removed == fd || closed_fd(removed));
  } else if (strcmp(argv[1], "--cwd") == 0) {
    int relative = open("input.bin", O_RDONLY);
    okay = read_data(relative);
    if (relative >= 0) close(relative);
  } else if (strcmp(argv[1], "--env") == 0) {
    const char* path = getenv("PATH");
    const char* marker = getenv("SPAWN_MARKER");
    okay = strcmp(argv[0], child_name) == 0 && path && strcmp(path, "/missing-spawn-path") == 0 &&
           marker && strcmp(marker, "exact replacement environment") == 0;
  }
  if (!okay) fprintf(stderr, "FAIL  exec4 spawn child: %s (pid%d)\n", argv[1], (int)getpid());
  return okay ? 42 : 41;
}

static int run_child(const char* program, char* const args[], char* const env[],
                     posix_spawn_file_actions_t* actions, int search, int expected, const char* name) {
  pid_t pid = -1;
  int result = search ? posix_spawnp(&pid, program, actions, NULL, args, env)
                      : posix_spawn(&pid, program, actions, NULL, args, env);
  int status = 0;
  pid_t waited = result == 0 && pid > 0 ? waitpid(pid, &status, 0) : -1;
  int okay = result == 0 && pid > 0 && waited == pid && WIFEXITED(status) &&
             WEXITSTATUS(status) == expected;
  if (!okay) printf("INFO  exec4 spawn: result%d pid%d waited%d status0x%x expected%d\n",
                    result, (int)pid, (int)waited, status, expected);
  check(okay, name);
  return okay ? (int)pid : -1;
}

static void add(int result) { check(result == 0, "register an actual ordered action"); }

static void fail_action(const char* self, const char* work, int kind, int parent_fd) {
  char marker[PATH_MAX], name[64];
  snprintf(name, sizeof(name), "must-not-exist-%d.bin", kind);
  if (!join(marker, sizeof(marker), work, name)) { check(0, "failure marker path fits"); return; }
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) { check(0, "init failing actions"); return; }
  switch (kind) {
    case 0: add(posix_spawn_file_actions_addopen(&actions, 30, "/missing-spawn-parent/input", O_RDONLY, 0)); break;
    case 1: add(posix_spawn_file_actions_adddup2(&actions, INT_MAX, 31)); break;
    case 2: add(posix_spawn_file_actions_adddup2(&actions, INT_MAX, INT_MAX)); break;
    case 3: add(posix_spawn_file_actions_addchdir_np(&actions, "/missing-spawn-directory")); break;
    case 4: add(posix_spawn_file_actions_addfchdir_np(&actions, INT_MAX)); break;
    case 5: add(posix_spawn_file_actions_addfchdir_np(&actions, parent_fd)); break;
  }
  add(posix_spawn_file_actions_addopen(&actions, 34, marker, O_WRONLY | O_CREAT | O_EXCL, 0600));
  char* args[] = {(char*)child_name, "--plain", NULL};
  static const char* const names[] = {
      "failed open returns spawn0 and a real child exit127",
      "missing dup2 source returns spawn0 and a real child exit127",
      "missing dup2 same-fd source returns spawn0 and a real child exit127",
      "failed chdir returns spawn0 and a real child exit127",
      "missing fchdir source returns spawn0 and a real child exit127",
      "regular-file fchdir returns spawn0 and a real child exit127"};
  run_child(self, args, environ, &actions, kind & 1, 127, names[kind]);
  check(access(marker, F_OK) == -1 && errno == ENOENT, "first failed action prevents later file creation");
  posix_spawn_file_actions_destroy(&actions);
}

int main(int argc, char** argv) {
  if (argc >= 2 && strncmp(argv[1], "--", 2) == 0) return child_main(argc, argv);
  if (argc != 3) { fprintf(stderr, "usage: exec4-spawn <actual-self-path> <work-directory>\n"); return 2; }
#if defined(__aarch64__)
  puts("exec4 spawn ABI: arm64-v8a");
#elif defined(__x86_64__)
  puts("exec4 spawn ABI: x86_64");
#else
#error Unsupported exec4 fixture ABI
#endif
  char before[PATH_MAX], after[PATH_MAX], a[PATH_MAX], b[PATH_MAX], input[PATH_MAX], output[PATH_MAX];
  if (!getcwd(before, sizeof(before)) || !join(a, sizeof(a), argv[2], "a-\xd7\x92") ||
      !join(b, sizeof(b), a, "b-\xd7\xa9") || !join(input, sizeof(input), b, "input.bin") ||
      !join(output, sizeof(output), b, "output.bin") || mkdir(a, 0700) || mkdir(b, 0700)) {
    perror("exec4 spawn directories"); return 1;
  }
  int out = open(input, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (out < 0 || write(out, data, sizeof(data) - 1) != (ssize_t)(sizeof(data) - 1) || close(out)) {
    perror("exec4 spawn input"); return 1;
  }
  int parent_fd = open(input, O_RDONLY | O_CLOEXEC);
  if (parent_fd < 0) { perror("exec4 spawn parent fd"); return 1; }
  int parent_flags = fcntl(parent_fd, F_GETFD);
  check(parent_flags == FD_CLOEXEC, "parent control starts close-on-exec");
  char parent_arg[32];
  snprintf(parent_arg, sizeof(parent_arg), "%d", parent_fd);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions)) return 1;
  add(posix_spawn_file_actions_addchdir_np(&actions, argv[2]));
  add(posix_spawn_file_actions_addchdir_np(&actions, "a-\xd7\x92"));
  add(posix_spawn_file_actions_addopen(&actions, 30, "b-\xd7\xa9/input.bin", O_RDONLY, 0));
  add(posix_spawn_file_actions_adddup2(&actions, 30, 31));
  add(posix_spawn_file_actions_addclose(&actions, 30));
  add(posix_spawn_file_actions_addopen(&actions, 32, "b-\xd7\xa9", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0));
  add(posix_spawn_file_actions_addfchdir_np(&actions, 32));
  add(posix_spawn_file_actions_addclose(&actions, 32));
  add(posix_spawn_file_actions_addclose(&actions, INT_MAX));
  add(posix_spawn_file_actions_addclose(&actions, parent_fd));
  add(posix_spawn_file_actions_addopen(&actions, 33, "output.bin", O_WRONLY | O_CREAT | O_EXCL, 0600));
  char* ordered[] = {(char*)child_name, "--ordered", parent_arg, NULL};
  int pid = run_child(argv[1], ordered, environ, &actions, 0, 42,
                      "ordered relative open/dup/close/chdir/fchdir runs in the real child");
  posix_spawn_file_actions_destroy(&actions);
  int child_pid = -1;
  FILE* recorded = fopen(output, "r");
  if (recorded) { if (fscanf(recorded, "%d", &child_pid) != 1) child_pid = -1; fclose(recorded); }
  check(pid > 0 && child_pid == pid, "returned PID is the ELF program PID written by that child");
  for (int kind = 0; kind < 6; ++kind) fail_action(argv[1], argv[2], kind, parent_fd);

  char* plain[] = {(char*)child_name, "--plain", NULL};
  posix_spawn_file_actions_init(&actions);
  add(posix_spawn_file_actions_addclose(&actions, INT_MAX));
  run_child(argv[1], plain, environ, &actions, 1, 42, "unopened positive close is ignored");
  posix_spawn_file_actions_destroy(&actions);
  char* closed[] = {(char*)child_name, "--closed", parent_arg, NULL};
  run_child(argv[1], closed, environ, NULL, 0, 42, "unmentioned inherited close-on-exec fd is closed");

  posix_spawn_file_actions_init(&actions);
  add(posix_spawn_file_actions_adddup2(&actions, parent_fd, parent_fd));
  char* keep[] = {(char*)child_name, "--keep", parent_arg, parent_arg, NULL};
  run_child(argv[1], keep, environ, &actions, 0, 42, "same-fd dup clears child close-on-exec");
  posix_spawn_file_actions_destroy(&actions);
  check(lseek(parent_fd, 0, SEEK_SET) == 0, "reset shared open-file offset before another child read");
  posix_spawn_file_actions_init(&actions);
  add(posix_spawn_file_actions_adddup2(&actions, parent_fd, 45));
  char* copy[] = {(char*)child_name, "--keep", "45", parent_arg, NULL};
  run_child(argv[1], copy, environ, &actions, 1, 42, "dup preserves an explicit copy and closes the flagged source");
  posix_spawn_file_actions_destroy(&actions);

  int directory = open(b, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  check(directory >= 0, "open a real inherited directory for fchdir");
  if (directory >= 0) {
    posix_spawn_file_actions_init(&actions);
    add(posix_spawn_file_actions_addfchdir_np(&actions, directory));
    char* cwd[] = {(char*)child_name, "--cwd", NULL};
    run_child(argv[1], cwd, environ, &actions, 0, 42, "fchdir uses an inherited flagged directory before exec closes it");
    posix_spawn_file_actions_destroy(&actions);
    close(directory);
  }
  posix_spawn_file_actions_init(&actions);
  add(posix_spawn_file_actions_addopen(&actions, 40, input, O_RDONLY | O_DIRECTORY, 0));
  run_child(argv[1], plain, environ, &actions, 0, 127, "ABI-specific O_DIRECTORY rejects a regular file in addopen");
  posix_spawn_file_actions_destroy(&actions);

  char marker[PATH_MAX];
  if (!join(marker, sizeof(marker), argv[2], "before-failed-exec.bin")) return 1;
  posix_spawn_file_actions_init(&actions);
  add(posix_spawn_file_actions_addopen(&actions, 40, marker, O_WRONLY | O_CREAT | O_EXCL, 0600));
  run_child("/missing-spawn-program", plain, environ, &actions, 0, 127,
            "failed exec returns spawn0 and real child127 after file actions");
  check(access(marker, F_OK) == 0, "successful file action precedes failed program lookup");
  posix_spawn_file_actions_destroy(&actions);

  char self_dir[PATH_MAX];
  if (strlen(argv[1]) >= sizeof(self_dir)) return 1;
  strcpy(self_dir, argv[1]);
  char* slash = strrchr(self_dir, '/');
  if (!slash) return 1;
  char* base = strrchr(argv[1], '/') + 1;
  *slash = 0;
  const char* old_path = getenv("PATH");
  char* saved_path = old_path ? strdup(old_path) : NULL;
  check(!old_path || saved_path, "preserve the caller PATH");
  check(setenv("PATH", self_dir, 1) == 0, "set an actual caller lookup PATH");
  char* replacement[] = {"PATH=/missing-spawn-path", "SPAWN_MARKER=exact replacement environment", NULL};
  char* env_args[] = {(char*)child_name, "--env", NULL};
  run_child(base, env_args, replacement, NULL, 1, 42,
            "spawnp searches caller PATH while passing exact argv and replacement envp");
  if (saved_path) { setenv("PATH", saved_path, 1); free(saved_path); } else unsetenv("PATH");
  check(getcwd(after, sizeof(after)) && strcmp(before, after) == 0 &&
            fcntl(parent_fd, F_GETFD) == parent_flags,
        "every child preserves the parent directory and descriptor flags");
  int stdin_flags = fcntl(0, F_GETFD);
  int saved_stdin = stdin_flags >= 0 ? dup(0) : -1;
  check(stdin_flags < 0 || saved_stdin >= 0, "save the parent stdin before the closed-slot control");
  if (stdin_flags < 0 || saved_stdin >= 0) {
    if (saved_stdin >= 0) fcntl(saved_stdin, F_SETFD, FD_CLOEXEC);
    close(0);
    char* no_stdin[] = {(char*)child_name, "--closed", "0", NULL};
    run_child(argv[1], no_stdin, environ, NULL, 0, 42,
              "unmentioned closed parent stdin stays EBADF in the child");
    if (!join(marker, sizeof(marker), argv[2], "after-closed-stdin-must-not-exist.bin")) return 1;
    posix_spawn_file_actions_init(&actions);
    add(posix_spawn_file_actions_adddup2(&actions, 0, 44));
    add(posix_spawn_file_actions_addopen(&actions, 40, marker, O_WRONLY | O_CREAT | O_EXCL, 0600));
    run_child(argv[1], plain, environ, &actions, 1, 127,
              "dup2 from closed parent stdin fails in a real child with127");
    check(access(marker, F_OK) == -1 && errno == ENOENT,
          "closed-stdin failure prevents later file creation");
    posix_spawn_file_actions_destroy(&actions);
    if (saved_stdin >= 0) {
      check(dup2(saved_stdin, 0) == 0 && fcntl(0, F_SETFD, stdin_flags) == 0,
            "restore the parent's original stdin and flags");
      close(saved_stdin);
    }
  }
  close(parent_fd);
  printf("%d test(s) failed\n", failed);
  return failed ? 1 : 0;
}
