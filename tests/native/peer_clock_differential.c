// One NDK-built ELF runs as-is on Android and under gesher-exec.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { THREADS = 257 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool capacity_release;
static bool busy_done;
static bool busy_release;
static volatile uint64_t workload = 1;

static void check(int error, const char* operation) {
  if (error == 0) return;
  fprintf(stderr, "%s: %d\n", operation, error);
  exit(2);
}

static int64_t nanos(clockid_t clock) {
  struct timespec sample;
  if (clock_gettime(clock, &sample) != 0) check(errno, "sample clock");
  return (int64_t)sample.tv_sec * 1000000000 + sample.tv_nsec;
}

static void observe(const char* name, int64_t value) {
  printf("PEER_CLOCK_V1 %s=%" PRId64 "\n", name, value);
}

static void pause_briefly(void) {
  const struct timespec interval = { .tv_nsec = 1000000 };
  nanosleep(&interval, NULL);
}

static void* busy_target(void* unused) {
  (void)unused;
  check(pthread_mutex_lock(&lock), "acquire busy target gate");
  check(pthread_mutex_unlock(&lock), "release busy target gate");
  int64_t cpu_start = nanos(CLOCK_THREAD_CPUTIME_ID);
  int64_t deadline = nanos(CLOCK_MONOTONIC) + 10000000000LL;
  while (nanos(CLOCK_THREAD_CPUTIME_ID) - cpu_start < 100000000 &&
         nanos(CLOCK_MONOTONIC) < deadline) {
    for (unsigned i = 0; i < 10000; ++i) workload = workload * 6364136223846793005ULL + 1;
  }
  check(pthread_mutex_lock(&lock), "acquire busy target completion lock");
  busy_done = true;
  check(pthread_cond_signal(&changed), "signal busy target completion");
  while (!busy_release)
    check(pthread_cond_wait(&changed, &lock), "wait for busy target release");
  check(pthread_mutex_unlock(&lock), "release busy target completion lock");
  return NULL;
}

static void observe_peer(void) {
  pthread_t target;
  check(pthread_mutex_lock(&lock), "close busy target gate");
  check(pthread_create(&target, NULL, busy_target, NULL), "create busy target");
  clockid_t peer;
  check(pthread_getcpuclockid(target, &peer), "acquire live peer clock");
  struct timespec resolution;
  check(clock_getres(peer, &resolution) == 0 ? 0 : errno, "get live peer resolution");
  observe("positive_resolution", resolution.tv_sec == 0 && resolution.tv_nsec > 0);
  unsigned same_id = 0;
  for (unsigned i = 0; i < 1024; ++i) {
    clockid_t again;
    if (pthread_getcpuclockid(target, &again) == 0 && again == peer) ++same_id;
  }
  observe("same_live_id", same_id);

  struct timespec deadline;
  check(clock_gettime(CLOCK_REALTIME, &deadline) == 0 ? 0 : errno, "get wait deadline");
  deadline.tv_sec += 15;
  int64_t peer_start = nanos(peer);
  int64_t caller_start = nanos(CLOCK_THREAD_CPUTIME_ID);
  while (!busy_done)
    check(pthread_cond_timedwait(&changed, &lock, &deadline), "wait for busy target");
  int64_t peer_delta = nanos(peer) - peer_start;
  int64_t caller_delta = nanos(CLOCK_THREAD_CPUTIME_ID) - caller_start;
  observe("peer_delta_ns", peer_delta);
  observe("caller_delta_ns", caller_delta);
  busy_release = true;
  check(pthread_cond_signal(&changed), "release busy target");
  check(pthread_mutex_unlock(&lock), "end busy target ownership");
  check(pthread_join(target, NULL), "join busy target");

  // Query only the retained clockid_t. A joined pthread_t is invalid on Android.
  // A Linux join can return just before the kernel releases the corresponding task.
  struct timespec sample;
  int64_t retirement_deadline = nanos(CLOCK_MONOTONIC) + 2000000000;
  int time_error;
  do {
    time_error = clock_gettime(peer, &sample) == 0 ? 0 : errno;
    if (time_error != 0 || nanos(CLOCK_MONOTONIC) >= retirement_deadline) break;
    pause_briefly();
  } while (true);
  observe("retired_gettime_errno", time_error);
  observe("retired_getres_errno", clock_getres(peer, &sample) == 0 ? 0 : errno);
}

static void* capacity_target(void* unused) {
  (void)unused;
  check(pthread_mutex_lock(&lock), "acquire capacity target gate");
  while (!capacity_release)
    check(pthread_cond_wait(&changed, &lock), "wait for capacity target release");
  check(pthread_mutex_unlock(&lock), "release capacity target gate");
  return NULL;
}

static void observe_capacity(void) {
  pthread_t workers[THREADS];
  clockid_t ids[THREADS];
  unsigned acquired = 0;
  unsigned distinct = 0;
  int first_error = 0;
  for (unsigned i = 0; i < THREADS; ++i)
    check(pthread_create(&workers[i], NULL, capacity_target, NULL), "create capacity target");
  for (unsigned i = 0; i < THREADS; ++i) {
    clockid_t clock;
    int error = pthread_getcpuclockid(workers[i], &clock);
    if (error != 0) {
      if (first_error == 0) first_error = error;
      continue;
    }
    unsigned j = 0;
    while (j < acquired && ids[j] != clock) ++j;
    distinct += j == acquired;
    ids[acquired++] = clock;
  }
  observe("capacity_acquired", acquired);
  observe("capacity_distinct", distinct);
  observe("capacity_first_errno", first_error);
  check(pthread_mutex_lock(&lock), "acquire capacity release gate");
  capacity_release = true;
  check(pthread_cond_broadcast(&changed), "release capacity targets");
  check(pthread_mutex_unlock(&lock), "end capacity release ownership");
  for (unsigned i = 0; i < THREADS; ++i)
    check(pthread_join(workers[i], NULL), "join capacity target");
}

int main(void) {
#if defined(__x86_64__)
  observe("elf_machine", 62);
#elif defined(__aarch64__)
  observe("elf_machine", 183);
#else
#error "This fixture supports x86-64 and arm64 only"
#endif
  observe_peer();
  observe_capacity();
  observe("complete", 1);
  return 0;
}
