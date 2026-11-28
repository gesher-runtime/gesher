/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct Shared {
  pthread_barrier_t barrier;
  int payload[2];
  int serial;
};
enum { kRounds = 64 };

static void check(int ok, const char* step) {
  if (ok) return;
  fprintf(stderr, "FAIL %s\n", step);
  _exit(1);
}

static void rounds(struct Shared* shared, int role) {
  for (int round = 1; round <= kRounds; ++round) {
    shared->payload[role] = round;
    int status = pthread_barrier_wait(&shared->barrier);
    check(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD, "publish round");
    if (status == PTHREAD_BARRIER_SERIAL_THREAD) ++shared->serial;
    check(shared->payload[1 - role] == round, "read peer publication");
    status = pthread_barrier_wait(&shared->barrier);
    check(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD, "retire both reads");
  }
}

int main(void) {
  alarm(12);
  char path[] = "./gesher-barrier.XXXXXX";
  int fd = mkstemp(path);
  check(fd >= 0, "create backing file");
  check(unlink(path) == 0 && ftruncate(fd, sizeof(struct Shared)) == 0, "size backing file");
  struct Shared* shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  check(shared != MAP_FAILED && close(fd) == 0, "map actual shared file");
  pthread_barrierattr_t attr;
  int mode = -1;
  check(pthread_barrierattr_init(&attr) == 0, "attribute init");
  check(pthread_barrierattr_getpshared(&attr, &mode) == 0 && mode == PTHREAD_PROCESS_PRIVATE,
        "default private");
  check(pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
        pthread_barrierattr_getpshared(&attr, &mode) == 0 && mode == PTHREAD_PROCESS_SHARED,
        "store shared attribute");
  check(pthread_barrier_init(&shared->barrier, NULL, 0) == EINVAL, "reject zero participants");
  check(pthread_barrier_init(&shared->barrier, NULL, 1) == 0 &&
        pthread_barrier_wait(&shared->barrier) == PTHREAD_BARRIER_SERIAL_THREAD &&
        pthread_barrier_destroy(&shared->barrier) == 0, "single participant is serial");
  check(pthread_barrier_init(&shared->barrier, &attr, 2) == 0, "shared barrier init");
  check(pthread_barrierattr_destroy(&attr) == 0, "attribute destroy");
  pid_t child = fork();
  check(child >= 0, "fork participant");
  if (child == 0) {
    alarm(12);
    rounds(shared, 1);
    _exit(0);
  }
  rounds(shared, 0);
  int status;
  check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "child exits after all rounds");
  check(shared->serial == kRounds, "one serial participant per round");
  check(pthread_barrier_destroy(&shared->barrier) == 0, "barrier destroy");
#ifdef __ANDROID__
  check(pthread_barrier_destroy(&shared->barrier) == EINVAL, "repeated destroy rejected");
#endif
  check(munmap(shared, sizeof(*shared)) == 0, "unmap barrier");
  printf("N29 Android pshared barrier: %d two-party publication rounds PASS\n", kRounds);
  return 0;
}
