#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
// ELF/Linux callers use their ordinary monotonic clock. Native PE fixtures
// override this with the Android clock used by Gesher's futex deadlines.
#ifndef FUTEX_TEST_CLOCK_GETTIME
#define FUTEX_TEST_CLOCK_GETTIME clock_gettime
#endif
typedef long (*FutexApi)(volatile int*, int, int, const struct timespec*, volatile int*, int);
static int futex_checks, futex_failures;
static FutexApi futex_wait_api, futex_wake_api;
static void futex_check(const char* label, int ok) {
  ++futex_checks; if (!ok) { ++futex_failures; printf("FAIL futex: %s (errno=%d)\n", label, errno); fflush(stdout); }
}
static long futex_call(volatile int* a, int op, int n, const struct timespec* t, volatile int* b, int m) {
  return futex_wake_api(a, op, n, t, b, m);
}
struct futex_waiter {
  volatile int gate;
  int flags, mask;
  long result;
  int error;
  pthread_t thread;
};
static void* futex_wait_thread(void* opaque) {
  struct futex_waiter* w = (struct futex_waiter*)opaque;
  struct timespec limit;
  FUTEX_TEST_CLOCK_GETTIME(CLOCK_MONOTONIC, &limit); limit.tv_sec += 2;
  w->result = futex_wait_api(&w->gate, FUTEX_WAIT_BITSET | w->flags, 0, &limit, NULL, w->mask);
  w->error = errno;
  return NULL;
}
static int futex_start(struct futex_waiter* w, volatile int* destination, int flags, int mask) {
  w->gate = 0; w->flags = flags; w->mask = mask; w->result = -99; w->error = 0;
  int created = pthread_create(&w->thread, NULL, futex_wait_thread, w);
  futex_check("create bounded waiter", created == 0); if (created) return 0;
  // Requeue provides an observable admission handshake: no timing-based guess
  // about whether the new thread has reached the kernel/production queue.
  long queued = 0;
  for (int i = 0; i < 500; ++i) {
    queued = futex_call(&w->gate, FUTEX_CMP_REQUEUE | flags, 0,
                        (const struct timespec*)(uintptr_t)1, destination, 0);
    if (queued != 0) break;
    struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
  }
  futex_check("waiter admitted and requeued exactly once", queued == 1);
  return 1;
}
static void futex_join(struct futex_waiter* w, int started) {
  if (!started) return;
  futex_check("join bounded waiter", pthread_join(w->thread, NULL) == 0);
  futex_check("selected waiter returned success", w->result == 0);
}
static void futex_inputs(void) {
  volatile int word = 0, target = 0;
  struct timespec bad[] = {{-1,0},{0,-1},{0,1000000000}};
  for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
    errno = 0;
    futex_check("invalid timeout precedes expected-value comparison", futex_call(&word,FUTEX_WAIT_PRIVATE,1,&bad[i],NULL,0) == -1 && errno == EINVAL);
  }
  struct timespec zero = {0,0}; errno = 0;
  futex_check("expected mismatch precedes expired deadline", futex_call(&word,FUTEX_WAIT_BITSET_PRIVATE,1,&zero,NULL,1) == -1 && errno == EAGAIN);
  errno = 0; futex_check("elapsed absolute deadline times out", futex_call(&word,FUTEX_WAIT_BITSET_PRIVATE,0,&zero,NULL,1) == -1 && errno == ETIMEDOUT);
  errno = 0; futex_check("empty bitset rejected", futex_call(&word,FUTEX_WAKE_BITSET_PRIVATE,1,NULL,NULL,0) == -1 && errno == EINVAL);
  errno = 0; futex_check("negative requeue count rejected", futex_call(&word,FUTEX_REQUEUE_PRIVATE,0,(const struct timespec*)(intptr_t)-1,&target,0) == -1 && errno == EINVAL);
  errno = 0; futex_check("negative requeue wake count rejected", futex_call(&word,FUTEX_REQUEUE_PRIVATE,-1,NULL,&target,0) == -1 && errno == EINVAL);
  errno = 0; futex_check("unaligned address rejected", futex_call((volatile int*)((char*)&word+1),FUTEX_WAKE_PRIVATE,1,NULL,NULL,0) == -1 && errno == EINVAL);
  errno = 0; futex_check("unsupported operation reports ENOSYS", futex_call(&word,127,1,NULL,NULL,0) == -1 && errno == ENOSYS);
  errno = 0; futex_check("relative realtime option follows Linux error", futex_call(&word,FUTEX_WAIT_PRIVATE | FUTEX_CLOCK_REALTIME,1,&zero,NULL,0) == -1 && errno == ENOSYS);
  errno = 0; futex_check("invalid realtime timeout is checked first", futex_call(&word,FUTEX_WAIT_PRIVATE | FUTEX_CLOCK_REALTIME,1,&bad[1],NULL,0) == -1 && errno == EINVAL);
  futex_check("empty wake reports zero actual waiters", futex_call(&word,FUTEX_WAKE_PRIVATE,INT_MAX,NULL,NULL,0) == 0);
  struct timespec start, finish, short_wait = {0,2000000};
  FUTEX_TEST_CLOCK_GETTIME(CLOCK_MONOTONIC,&start); errno = 0;
  long rc = futex_call(&word,FUTEX_WAIT_PRIVATE,0,&short_wait,NULL,0); int e = errno;
  FUTEX_TEST_CLOCK_GETTIME(CLOCK_MONOTONIC,&finish);
  int64_t elapsed = (int64_t)(finish.tv_sec-start.tv_sec)*1000000000+finish.tv_nsec-start.tv_nsec;
  futex_check("relative timeout has its full duration", rc == -1 && e == ETIMEDOUT && elapsed >= 2000000);
  FUTEX_TEST_CLOCK_GETTIME(CLOCK_MONOTONIC,&start);
  struct timespec deadline = start;
  deadline.tv_nsec += 2000000;
  if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
  errno = 0;
  rc = futex_call(&word,FUTEX_WAIT_BITSET_PRIVATE,0,&deadline,NULL,1); e = errno;
  FUTEX_TEST_CLOCK_GETTIME(CLOCK_MONOTONIC,&finish);
  elapsed = (int64_t)(finish.tv_sec-start.tv_sec)*1000000000+finish.tv_nsec-start.tv_nsec;
  futex_check("absolute timeout shares the monotonic origin and full duration",
              rc == -1 && e == ETIMEDOUT && elapsed >= 2000000);
}
static void futex_selection(int flags) {
  volatile int word = 0, moved = 0;
  struct futex_waiter one, two;
  int first = futex_start(&one,&word,flags,1), second = futex_start(&two,&word,flags,2);
  futex_check("private and ordinary queues stay distinct", futex_call(&word,FUTEX_WAKE | (flags ^ FUTEX_PRIVATE_FLAG),INT_MAX,NULL,NULL,0) == 0);
  futex_check("disjoint wake mask selects none", futex_call(&word,FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,4) == 0);
  errno = 0;
  futex_check("compare-requeue mismatch leaves waiters", futex_call(&word,FUTEX_CMP_REQUEUE | flags,0,(const struct timespec*)(uintptr_t)2,&moved,1) == -1 && errno == EAGAIN);
  futex_check("compare-requeue reports moved count", futex_call(&word,FUTEX_CMP_REQUEUE | flags,0,(const struct timespec*)(uintptr_t)2,&moved,0) == 2);
  futex_check("requeued waiters leave original address", futex_call(&word,FUTEX_WAKE | flags,INT_MAX,NULL,NULL,0) == 0);
  futex_check("requeue preserves key mode", futex_call(&moved,FUTEX_WAKE | (flags ^ FUTEX_PRIVATE_FLAG),INT_MAX,NULL,NULL,0) == 0);
  futex_check("requeue preserves first waiter's mask", futex_call(&moved,FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,1) == 1);
  futex_check("already selected mask reports zero", futex_call(&moved,FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,1) == 0);
  futex_check("ordinary wake reports remaining exact count", futex_call(&moved,FUTEX_WAKE | flags,INT_MAX,NULL,NULL,0) == 1);
  futex_join(&one,first); futex_join(&two,second);
  for (int n = -1; n <= 0; ++n) {
    struct futex_waiter w; int started = futex_start(&w,&word,flags,1);
    futex_check("Linux nonpositive wake count can select one", futex_call(&word,FUTEX_WAKE | flags,n,NULL,NULL,0) == 1);
    futex_join(&w,started);
  }
  struct futex_waiter w; int started = futex_start(&w,&word,flags,1);
  futex_check("same-address requeue is supported", futex_call(&word,FUTEX_REQUEUE | flags,0,(const struct timespec*)(uintptr_t)1,&word,0) == 1);
  futex_check("same-address requeue retains waiter", futex_call(&word,FUTEX_WAKE | flags,1,NULL,NULL,0) == 1);
  futex_join(&w,started);
}
struct futex_mover {
  volatile int* source;
  volatile int* target;
  volatile int* stop;
  volatile int* progressed;
  int flags, iterations;
  unsigned long moves, failures;
  pthread_t thread;
};
static void* futex_move_thread(void* opaque) {
  struct futex_mover* m = (struct futex_mover*)opaque;
  for (int i = 0; m->iterations < 0 || i < m->iterations; ++i) {
    if (__atomic_load_n(m->stop, __ATOMIC_ACQUIRE)) break;
    long result = futex_call(m->source,FUTEX_CMP_REQUEUE | m->flags,0,
                            (const struct timespec*)(uintptr_t)INT_MAX,m->target,0);
    if (result < 0) ++m->failures; else m->moves += (unsigned long)result;
    if (result > 0) __atomic_store_n(m->progressed,1,__ATOMIC_RELEASE);
  }
  return NULL;
}
static void futex_requeue_race(int flags, int finish, int offset) {
  // Include near addresses and an offset that collides in the production
  // bucket hash. The assertions concern only the public requeue contract.
  volatile int words[8193] = {0};
  volatile int stop = 0, progressed = 0;
  struct futex_waiter waiters[4]; int started[4];
  for (int i = 0; i < 4; ++i) started[i] = futex_start(&waiters[i],&words[0],flags,1 << i);
  struct futex_mover movers[2] = {
    {&words[0],&words[offset],&stop,&progressed,flags,finish ? -1 : 2000,0,0,0},
    {&words[offset],&words[0],&stop,&progressed,flags,finish ? -1 : 2000,0,0,0}
  };
  int moving[2];
  for (int i = 0; i < 2; ++i) {
    moving[i] = pthread_create(&movers[i].thread,NULL,futex_move_thread,&movers[i]) == 0;
    futex_check("create opposing requeue worker",moving[i]);
  }
  if (finish == 1) {
    // Join while both requeue directions remain active: each waiter must
    // remove itself from whichever queue currently owns its stack node.
    for (int i = 0; i < 4; ++i) if (started[i]) {
      futex_check("join waiter timing out during requeue",pthread_join(waiters[i].thread,NULL) == 0);
      futex_check("migration preserves timeout result",waiters[i].result == -1 && waiters[i].error == ETIMEDOUT);
    }
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
  } else if (finish == 2) {
    // Race wake/return with migration too. A waiter may move between the two
    // wake calls; retry only when neither queue selected it.
    for (int attempt = 0; attempt < 500 && !__atomic_load_n(&progressed,__ATOMIC_ACQUIRE); ++attempt) {
      struct timespec pause = {0,1000000}; nanosleep(&pause,NULL);
    }
    futex_check("migration starts before concurrent wake",__atomic_load_n(&progressed,__ATOMIC_ACQUIRE));
    for (int i = 0; i < 4; ++i) {
      long selected = 0;
      for (int attempt = 0; attempt < 200 && selected == 0; ++attempt) {
        selected = futex_call(&words[0],FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,1 << i) +
                   futex_call(&words[offset],FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,1 << i);
        if (selected == 0) { struct timespec pause = {0,1000000}; nanosleep(&pause,NULL); }
      }
      futex_check("wake during migration selects each mask once",selected == 1);
      futex_join(&waiters[i],started[i]);
    }
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
  }
  for (int i = 0; i < 2; ++i) if (moving[i]) {
    futex_check("opposing requeues complete without deadlock",pthread_join(movers[i].thread,NULL) == 0);
    futex_check("opposing requeues preserve compare values",movers[i].failures == 0);
  }
  futex_check("requeue race actually moved waiters",movers[0].moves + movers[1].moves > 0);
  for (int i = 0; i < 4; ++i) {
    long selected = futex_call(&words[0],FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,1 << i) +
                    futex_call(&words[offset],FUTEX_WAKE_BITSET | flags,INT_MAX,NULL,NULL,1 << i);
    futex_check(finish ? "retired stack node is removed from both queues" :
                        "opposing requeues retain each mask exactly once",selected == (finish ? 0 : 1));
    if (!finish) futex_join(&waiters[i],started[i]);
  }
}
static void futex_concurrency(void) {
  for (int round = 0; round < 8; ++round) {
    volatile int word = 0;
    struct futex_waiter waiters[4]; int started[4];
    for (int i = 0; i < 4; ++i) started[i] = futex_start(&waiters[i],&word,FUTEX_PRIVATE_FLAG,1);
    futex_check("wake count is capped to requested two", futex_call(&word,FUTEX_WAKE_PRIVATE,2,NULL,NULL,0) == 2);
    futex_check("second wake sees exactly two remaining", futex_call(&word,FUTEX_WAKE_PRIVATE,INT_MAX,NULL,NULL,0) == 2);
    for (int i = 0; i < 4; ++i) futex_join(&waiters[i],started[i]);
    futex_check("retired stack waiters are not retained", futex_call(&word,FUTEX_WAKE_PRIVATE,INT_MAX,NULL,NULL,0) == 0);
  }
  for (int flags = 0; flags <= FUTEX_PRIVATE_FLAG; flags += FUTEX_PRIVATE_FLAG) {
    futex_requeue_race(flags,0,1);
    futex_requeue_race(flags,0,8192);
    futex_requeue_race(flags,1,1);
    futex_requeue_race(flags,2,1);
  }
}
static int futex_run(FutexApi wait_api, FutexApi wake_api, int concurrency) {
  futex_wait_api = wait_api; futex_wake_api = wake_api;
  futex_inputs(); futex_selection(0); futex_selection(FUTEX_PRIVATE_FLAG);
  if (concurrency) futex_concurrency();
  printf("futex_contract: %d checks, %d failures\n",futex_checks,futex_failures);
  if (!futex_failures) puts("futex_contract: all passed");
  return futex_failures;
}
