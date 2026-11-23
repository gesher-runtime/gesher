// Linux contract reference, not a Windows/ART execution substitute.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct actor { atomic_int ready, stop, tid; };
static _Noreturn void fail(const char *message) {
  fprintf(stderr, "FAIL Linux N28: %s (errno=%d)\n", message, errno);
  fflush(NULL); _Exit(1);
}
static int64_t nanos(clockid_t clock) {
  struct timespec value;
  if (clock_gettime(clock, &value)) fail("read live clock");
  return (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static void *work(void *raw) {
  struct actor *actor = raw;
  atomic_store(&actor->tid, (int)syscall(SYS_gettid));
  atomic_store(&actor->ready, 1);
  while (!atomic_load(&actor->stop)) atomic_signal_fence(memory_order_seq_cst);
  return NULL;
}
int main(void) {
  alarm(120);
  void *library = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!library) fail("load libc reference");
  int (*symbol_id)(pthread_t, clockid_t *) = dlsym(library, "pthread_getcpuclockid");
  int (*symbol_read)(clockid_t, struct timespec *) = dlsym(library, "clock_gettime");
  if (!symbol_id || !symbol_read) fail("literal libc symbols");
  int tids[64] = {0};
  for (int row = 0; row < 64; ++row) {
    struct actor actor = {0}; pthread_t thread;
    if (pthread_create(&thread, NULL, work, &actor)) fail("create target");
    int64_t deadline = nanos(CLOCK_MONOTONIC) + 5000000000LL;
    while (!atomic_load(&actor.ready) && nanos(CLOCK_MONOTONIC) < deadline) usleep(100);
    if (!atomic_load(&actor.ready)) fail("target readiness deadline");
    int tid = atomic_load(&actor.tid);
    for (int old = 0; old < row; ++old) if (tids[old] == tid) fail("reference observed TID reuse");
    tids[row] = tid;
    clockid_t direct, dynamic;
    if (pthread_getcpuclockid(thread, &direct) || symbol_id(thread, &dynamic) || direct != dynamic ||
        ~(direct >> 3) != tid || ((unsigned)direct & 7) != 6) fail("Linux target encoding and literal-symbol identity");
    int64_t before = nanos(direct);
    struct timespec literal;
    if (symbol_read(dynamic, &literal)) fail("literal clock reader");
    int64_t sampled = (int64_t)literal.tv_sec * 1000000000 + literal.tv_nsec;
    if (sampled < before || sampled > nanos(direct)) fail("direct/dynamic reader bracket");
    if (row == 0) {
      int64_t caller_before = nanos(CLOCK_THREAD_CPUTIME_ID);
      usleep(250000);
      int64_t peer_delta = nanos(direct) - before;
      int64_t caller_delta = nanos(CLOCK_THREAD_CPUTIME_ID) - caller_before;
      printf("LINUX_MEASURE peer_ns=%" PRId64 " caller_ns=%" PRId64 "\n", peer_delta, caller_delta);
      if (peer_delta <= 1000000 || peer_delta <= caller_delta) fail("sleeping caller observes busy target");
    }
    atomic_store(&actor.stop, 1);
    if (pthread_join(thread, NULL)) fail("join target");
    // The kernel can release the task after the join futex has been cleared.
    // Wait for both kernel operations to reject this ID before any new target;
    // do not dereference the now-invalid glibc pthread_t or race /proc reads.
    deadline = nanos(CLOCK_MONOTONIC) + 5000000000LL;
    int released = 0;
    do {
      struct timespec value = {123, 456}; errno = 0;
      int time_result = symbol_read(direct, &value), time_error = errno;
      errno = 0;
      int resolution_result = clock_getres(direct, &value), resolution_error = errno;
      errno = 0;
      int task_result = (int)syscall(SYS_tgkill, getpid(), tid, 0), task_error = errno;
      if (time_result == -1 && time_error == EINVAL && resolution_result == -1 &&
          resolution_error == EINVAL && task_result == -1 && task_error == ESRCH) { released = 1; break; }
      if (time_result != 0 && (time_result != -1 || time_error != EINVAL)) fail("unexpected post-join clock result");
      usleep(100);
    } while (nanos(CLOCK_MONOTONIC) < deadline);
    if (!released) fail("kernel target release deadline");
    printf("LINUX_RELEASE row=%d tid=%d clock=%d time=EINVAL res=EINVAL task=ESRCH\n", row, tid, direct);
  }
  if (dlclose(library)) fail("close libc reference");
  puts("PASS Linux N28: busy peer, literal symbols, encoded identity and 64 final task releases");
  return 0;
}
