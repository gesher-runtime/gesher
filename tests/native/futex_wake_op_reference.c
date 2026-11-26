/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void Require(int ok, const char* why) {
  if (!ok) {
    fprintf(stderr, "%s: errno=%d\n", why, errno);
    exit(1);
  }
}

static long WakeOperation(int* first, int* second, int command, int count, int count2,
                          unsigned encoded) {
  return syscall(SYS_futex, first, command, count, count2, second, encoded);
}

#include "futex_wake_op_test.inc"

struct Words {
  int ready;
  int first;
  int second;
};

static pid_t Spawn(int fd, int use_second) {
  pid_t pid = fork();
  Require(pid >= 0, "fork waiter");
  if (pid != 0) return pid;
  struct Words* view = mmap(NULL, sizeof(*view), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  Require(view != MAP_FAILED, "child maps backing file");
  __atomic_add_fetch(&view->ready, 1, __ATOMIC_RELEASE);
  struct timespec limit;
  Require(clock_gettime(CLOCK_MONOTONIC, &limit) == 0, "read monotonic time");
  limit.tv_sec += 5;
  int* key = use_second ? &view->second : &view->first;
  long result;
  do {
    const int expected = use_second == 2 ? __atomic_load_n(key, __ATOMIC_SEQ_CST) : 0;
    result = syscall(SYS_futex, key, FUTEX_WAIT_BITSET, expected, &limit, NULL, 4);
  } while (use_second == 2 && result == -1 && errno == EAGAIN);
  Require(result == 0, "child receives real wake");
  _exit(0);
}

static void Reap(pid_t pid) {
  int outcome = 0;
  Require(waitpid(pid, &outcome, 0) == pid && WIFEXITED(outcome) && WEXITSTATUS(outcome) == 0,
          "waiter exit");
}

static void WakeBoth(struct Words* view, int* first, int* second) {
  const struct timespec delay = {0, 1000000};
  long woken = 0;
  for (int retry = 0; retry < 3500; ++retry) {
    if (__atomic_load_n(&view->ready, __ATOMIC_ACQUIRE) == 2) {
      const long count = WakeOperation(first, second, FUTEX_WAKE_OP, 1, 1,
                                      FUTEX_OP(FUTEX_OP_ADD, 0, FUTEX_OP_CMP_EQ, 0));
      Require(count >= 0 && count <= 2 && woken + count <= 2, "wake both queues");
      if ((woken += count) == 2) return;
    }
    nanosleep(&delay, NULL);
  }
  Require(0, "both shared waiters enrolled and woke");
}

int main(int argc, char** argv) {
  Require(argc == 2, "pass an unoccupied backing file path");
  CheckWakeOpWords();
  int fd = open(argv[1], O_RDWR | O_CREAT | O_EXCL, 0600);
  Require(fd >= 0, "create own backing file");
  Require(unlink(argv[1]) == 0, "unlink fixture backing file");
  Require(ftruncate(fd, sizeof(struct Words)) == 0, "size backing file");
  struct Words* view = mmap(NULL, sizeof(*view), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct Words* alias = mmap(NULL, sizeof(*alias), PROT_READ, MAP_SHARED, fd, 0);
  Require(view != MAP_FAILED && alias != MAP_FAILED, "map independent file aliases");
  Require(WakeOperation(&view->first, &alias->second, FUTEX_WAKE_OP, 1, 1,
                        FUTEX_OP(FUTEX_OP_SET, 99, FUTEX_OP_CMP_EQ, 0)) == -1 &&
          errno == EFAULT && view->second == 0, "failed update leaves shared word unchanged");

  pid_t a = Spawn(fd, 0), b = Spawn(fd, 1);
  WakeBoth(view, &alias->first, &view->second);
  Reap(a);
  Reap(b);
  Require(WakeOperation(&view->first, &view->second, FUTEX_WAKE_OP, 1, 1,
                        FUTEX_OP(FUTEX_OP_SET, 27, FUTEX_OP_CMP_EQ, 0)) == 0 &&
          alias->second == 27 && view->first == 0, "second word updated through the alias");

  view->ready = 0;
  a = Spawn(fd, 0);
  b = Spawn(fd, 0);
  WakeBoth(view, &alias->first, &view->first);
  Reap(a);
  Reap(b);

  view->ready = view->second = 0;
  a = Spawn(fd, 2);
  int local = 0, woke = 0;
  const struct timespec delay = {0, 1000000};
  for (int attempt = 0; attempt < 2000; ++attempt) {
    if (__atomic_load_n(&view->ready, __ATOMIC_ACQUIRE) != 0) {
      const int old = __atomic_load_n(&view->second, __ATOMIC_SEQ_CST);
      const long n = WakeOperation(&local, &view->second, FUTEX_WAKE_OP, 1, 1,
                                   FUTEX_OP(FUTEX_OP_XOR, 1, FUTEX_OP_CMP_EQ, old));
      Require(n == 0 || n == 1, "conditional wake uses the original second word");
      if ((woke = (int)n) != 0) break;
    }
    nanosleep(&delay, NULL);
  }
  Require(woke == 1, "conditional waiter was selected");
  Reap(a);

  Require(munmap(view, sizeof(*view)) == 0 && munmap(alias, sizeof(*alias)) == 0 &&
          close(fd) == 0, "retire fixture resources");
  puts("N29 FUTEX_WAKE_OP atomic operations and two cross-process keys PASS");
  return 0;
}
