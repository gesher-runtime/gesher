// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// The same public wait corpus against Linux's real process/thread APIs. The
// two child modes are explicit test processes, not production substitutions.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
static int checks, failures;
static void ok(const char* name, int good, const char* detail) {
  ++checks;
  if (!good) { ++failures; fprintf(stderr, "FAIL %s: %s\n", name, detail); }
}
#include "child_wait_contract.inc"
static void exit_handler(int signal_number) { (void)signal_number; _exit(43); }
int main(int argc, char** argv) {
  if (argc > 2 && strcmp(argv[1], "exit") == 0) return atoi(argv[2]);
  if (argc > 1 && strcmp(argv[1], "sigloop") == 0) {
    signal(SIGTERM, exit_handler);
    puts("child-ready"); fflush(stdout);
    for (;;) pause();
  }
  child_wait_notification_contract(argv[0]);
  printf("Linux public child wait: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
