#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
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
  clock_gettime(CLOCK_MONOTONIC, &limit); limit.tv_sec += 2;
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
  clock_gettime(CLOCK_MONOTONIC,&start); errno = 0;
  long rc = futex_call(&word,FUTEX_WAIT_PRIVATE,0,&short_wait,NULL,0); int e = errno;
  clock_gettime(CLOCK_MONOTONIC,&finish);
  int64_t elapsed = (int64_t)(finish.tv_sec-start.tv_sec)*1000000000+finish.tv_nsec-start.tv_nsec;
  futex_check("relative timeout has its full duration", rc == -1 && e == ETIMEDOUT && elapsed >= 2000000);
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
}
static int futex_run(FutexApi wait_api, FutexApi wake_api, int concurrency) {
  futex_wait_api = wait_api; futex_wake_api = wake_api;
  futex_inputs(); futex_selection(0); futex_selection(FUTEX_PRIVATE_FLAG);
  if (concurrency) futex_concurrency();
  printf("futex_contract: %d checks, %d failures\n",futex_checks,futex_failures);
  if (!futex_failures) puts("futex_contract: all passed");
  return futex_failures;
}
