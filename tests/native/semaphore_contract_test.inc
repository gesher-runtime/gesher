// Public Android semaphore contracts, shared with the ELF libc suite.
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef int (*sem_init_fn)(sem_t*, int, unsigned);
typedef int (*sem_action_fn)(sem_t*);
typedef int (*sem_value_fn)(sem_t*, int*);
typedef int (*sem_timed_fn)(sem_t*, const struct timespec*);
struct semaphore_api {
  sem_init_fn init;
  sem_action_fn post, wait, trywait, destroy;
  sem_value_fn value;
  sem_timed_fn timed, monotonic;
};
static struct semaphore_api get_semaphore_api(void) {
  struct semaphore_api api;
  api.init = (sem_init_fn)dlsym(RTLD_DEFAULT, "sem_init");
  api.post = (sem_action_fn)dlsym(RTLD_DEFAULT, "sem_post");
  api.wait = (sem_action_fn)dlsym(RTLD_DEFAULT, "sem_wait");
  api.trywait = (sem_action_fn)dlsym(RTLD_DEFAULT, "sem_trywait");
  api.destroy = (sem_action_fn)dlsym(RTLD_DEFAULT, "sem_destroy");
  api.value = (sem_value_fn)dlsym(RTLD_DEFAULT, "sem_getvalue");
  api.timed = (sem_timed_fn)dlsym(RTLD_DEFAULT, "sem_timedwait");
  api.monotonic = (sem_timed_fn)dlsym(RTLD_DEFAULT, "sem_timedwait_monotonic_np");
  return api;
}
static int semaphore_api_ready(struct semaphore_api api) {
  int ready = api.init && api.post && api.wait && api.trywait && api.destroy && api.value &&
              api.timed && api.monotonic;
  check("semaphore: public exports", ready);
  return ready;
}
struct semaphore_post_work {
  sem_action_fn post;
  sem_t* semaphore;
  int delayed, result, error;
};
static void* semaphore_post_worker(void* context) {
  struct semaphore_post_work* work = (struct semaphore_post_work*)context;
  if (work->delayed) {
    struct timespec pause = {0, 30000000};
    nanosleep(&pause, NULL);
  }
  errno = 0;
  work->result = work->post(work->semaphore);
  work->error = errno;
  return NULL;
}

static void test_semaphore_limits(void) {
  struct semaphore_api api = get_semaphore_api();
  if (!semaphore_api_ready(api)) return;
  const unsigned android_max = 0x3fffffff;
  sem_t semaphore;
  unsigned char previous[sizeof(semaphore)];
  memset(&semaphore, 0xa5, sizeof(semaphore));
  memcpy(previous, &semaphore, sizeof(semaphore));
  errno = 0;
  check("semaphore: reject max+1 without modifying storage",
        api.init(&semaphore, 0, android_max + 1) == -1 && errno == EINVAL &&
        memcmp(previous, &semaphore, sizeof(semaphore)) == 0);
  errno = 0;
  check("semaphore: reject UINT_MAX without modifying storage",
        api.init(&semaphore, 0, UINT32_MAX) == -1 && errno == EINVAL &&
        memcmp(previous, &semaphore, sizeof(semaphore)) == 0);
  check("semaphore: initialize at Android maximum", api.init(&semaphore, 0, android_max) == 0);
  int value = -1;
  check("semaphore: getvalue reports maximum", api.value(&semaphore, &value) == 0 && value == (int)android_max);
  errno = 0;
  check("semaphore: overflowing post returns EOVERFLOW", api.post(&semaphore) == -1 && errno == EOVERFLOW);
  check("semaphore: overflow leaves count intact", api.value(&semaphore, &value) == 0 && value == (int)android_max);
  check("semaphore: count remains usable after overflow", api.trywait(&semaphore) == 0 &&
        api.value(&semaphore, &value) == 0 && value == (int)android_max - 1);
  check("semaphore: post back to maximum succeeds", api.post(&semaphore) == 0 &&
        api.value(&semaphore, &value) == 0 && value == (int)android_max);
  api.destroy(&semaphore);

  api.init(&semaphore, 0, android_max - 1);
  struct semaphore_post_work work[8];
  pthread_t threads[8];
  int started[8];
  int success = 0, overflow = 0, created = 0;
  for (int i = 0; i < 8; ++i) {
    work[i].post = api.post;
    work[i].semaphore = &semaphore;
    work[i].delayed = 0;
    work[i].result = 99;
    work[i].error = 0;
    started[i] = pthread_create(&threads[i], NULL, semaphore_post_worker, &work[i]) == 0;
    created += started[i];
  }
  for (int i = 0; i < 8; ++i) {
    if (!started[i]) continue;
    pthread_join(threads[i], NULL);
    success += work[i].result == 0;
    overflow += work[i].result == -1 && work[i].error == EOVERFLOW;
  }
  check("semaphore: concurrent posts respect maximum atomically", created == 8 && success == 1 && overflow == 7);
  check("semaphore: concurrent count remains maximum", api.value(&semaphore, &value) == 0 && value == (int)android_max);
  api.destroy(&semaphore);
}

static void test_semaphore_waits(void) {
  struct semaphore_api api = get_semaphore_api();
  if (!semaphore_api_ready(api)) return;
  sem_t semaphore;
  api.init(&semaphore, 0, 1);
  struct timespec invalid = {0, 1000000000};
  check("semaphore: available token precedes timeout validation", api.timed(&semaphore, &invalid) == 0);
  errno = 0;
  check("semaphore: unavailable token validates nanoseconds", api.timed(&semaphore, &invalid) == -1 && errno == EINVAL);
  struct timespec expired = {INT64_MIN, 0};
  errno = 0;
  check("semaphore: distant past is a timeout", api.timed(&semaphore, &expired) == -1 && errno == ETIMEDOUT);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_nsec += 10000000;
  if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
  errno = 0;
  check("semaphore: finite empty wait times out", api.timed(&semaphore, &deadline) == -1 && errno == ETIMEDOUT);
  api.destroy(&semaphore);
  for (int mode = 0; mode < 3; ++mode) {
    api.init(&semaphore, 0, 0);
    struct semaphore_post_work work = {api.post, &semaphore, 1, 99, 0};
    pthread_t thread;
    int started = pthread_create(&thread, NULL, semaphore_post_worker, &work) == 0;
    check("semaphore: start controlled producer", started);
    if (started) {
      struct timespec distant = {INT64_MAX, 999999999};
      int result = mode == 0 ? api.wait(&semaphore)
                   : mode == 1 ? api.timed(&semaphore, &distant)
                               : api.monotonic(&semaphore, &distant);
      pthread_join(thread, NULL);
      check(mode == 0 ? "semaphore: untimed wait receives token"
            : mode == 1 ? "semaphore: distant realtime deadline receives token"
                        : "semaphore: distant monotonic deadline receives token",
            result == 0 && work.result == 0);
      int value = -1;
      check("semaphore: successful wait consumes exactly one token", api.value(&semaphore, &value) == 0 && value == 0);
    }
    api.destroy(&semaphore);
  }
}

static void test_semaphore_contracts(void) {
  test_semaphore_limits();
  test_semaphore_waits();
}
