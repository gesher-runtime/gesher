// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Called from the host regression and the real ART APK after host setenv.
#include <dlfcn.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

int Exec4Environment(const char* child, const char* expected) {
  char*** alias = dlsym(RTLD_DEFAULT, "__environ");
  if (alias != &environ) return 1;
  const char* value = NULL;
  for (char** e = environ; e != NULL && *e != NULL; ++e) {
    if (strncmp(*e, "GESHER_EXEC4_ENV=", 17) == 0) value = *e + 17;
  }
  const char* got = getenv("GESHER_EXEC4_ENV");
  if (expected ? value == NULL || got == NULL || strcmp(value, expected) != 0 ||
                     strcmp(got, expected) != 0
               : value != NULL || got != NULL)
    return 2;
  int p[2];
  if (pipe(p) != 0) return 3;
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(p[0]);
    close(p[1]);
    return 4;
  }
  int error = posix_spawn_file_actions_adddup2(&actions, p[1], STDOUT_FILENO);
  if (!error) error = posix_spawn_file_actions_addclose(&actions, p[0]);
  if (!error) error = posix_spawn_file_actions_addclose(&actions, p[1]);
  char* args[] = {(char*)child, (char*)"env", (char*)"GESHER_EXEC4_ENV", NULL};
  pid_t pid = -1;
  if (!error) error = posix_spawn(&pid, child, &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(p[1]);
  if (error != 0) {
    close(p[0]);
    return 5;
  }
  char output[128] = {0};
  size_t count = 0;
  ssize_t n;
  while (count + 1 < sizeof(output) &&
         (n = read(p[0], output + count, sizeof(output) - count - 1)) > 0)
    count += n;
  close(p[0]);
  int status = -1;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return 6;
  if (count == 0 || output[count - 1] != '\n') return 7;
  output[--count] = 0;
  if (strcmp(output, expected ? expected : "(null)") != 0) return 8;
  return 0;
}

int Exec4EnvironmentMutations(const char* child, const char* unused) {
  (void)unused;
  if (setenv("GESHER_EXEC4_ENV", "guest-setenv", 1) != 0 ||
      Exec4Environment(child, "guest-setenv") != 0)
    return 1;
  if (setenv("GESHER_EXEC4_ENV", "ignored", 0) != 0 || Exec4Environment(child, "guest-setenv") != 0)
    return 2;
  static char assignment[] = "GESHER_EXEC4_ENV=guest-putenv";
  if (putenv(assignment) != 0 || Exec4Environment(child, "guest-putenv") != 0) return 3;
  if (unsetenv("GESHER_EXEC4_ENV") != 0 || Exec4Environment(child, NULL) != 0) return 4;
  puts(
      "exec4: PASS environ aliases, guest setenv/putenv/unsetenv and "
      "posix_spawn file actions");
  return 0;
}
