// Public calls shared by the Linux reference, the extracted production
// adapters, and Android LP64 ELF fixtures. Long waits use controlled clocks
// in the host envelope; no public test deliberately sleeps for days.
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef ORDINARY_CLOCK_NANOSLEEP
#define ORDINARY_CLOCK_NANOSLEEP clock_nanosleep
#endif
#ifndef ORDINARY_SCHED_GETAFFINITY
#define ORDINARY_SCHED_GETAFFINITY sched_getaffinity
#endif

static int (*volatile ordinary_sleep_call)(clockid_t, int, const struct timespec*,
                                          struct timespec*) = ORDINARY_CLOCK_NANOSLEEP;
static int (*volatile ordinary_affinity_call)(pid_t, size_t, cpu_set_t*) = ORDINARY_SCHED_GETAFFINITY;

static int64_t ordinary_elapsed_ns(const struct timespec* first, const struct timespec* last) {
  return (int64_t)(last->tv_sec - first->tv_sec) * 1000000000 +
         last->tv_nsec - first->tv_nsec;
}

static void test_clock_sleep_inputs(void) {
  const struct timespec invalid[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    struct timespec remain = {111, 222};
    errno = EDOM;
    int result = ordinary_sleep_call(CLOCK_MONOTONIC, 0, &invalid[i], &remain);
    check("clock_nanosleep invalid timespec returns EINVAL and preserves errno",
          result == EINVAL && errno == EDOM);
    check("clock_nanosleep invalid timespec leaves remainder unchanged",
          remain.tv_sec == 111 && remain.tv_nsec == 222);
  }
  const int invalid_clocks[] = {-1, 999, CLOCK_THREAD_CPUTIME_ID};
  struct timespec zero = {0, 0};
  for (size_t i = 0; i < sizeof(invalid_clocks) / sizeof(invalid_clocks[0]); ++i) {
    errno = EDOM;
    int result = ordinary_sleep_call(invalid_clocks[i], 0, &zero, NULL);
    check("clock_nanosleep invalid clock returns EINVAL and preserves errno",
          result == EINVAL && errno == EDOM);
  }
  errno = EDOM;
  check("clock_nanosleep null request returns EFAULT",
        ordinary_sleep_call(CLOCK_MONOTONIC, 0, NULL, NULL) == EFAULT && errno == EDOM);
  errno = EDOM;
  check("clock_nanosleep zero request and unused Linux flag bit succeed",
        ordinary_sleep_call(CLOCK_MONOTONIC, 2, &zero, NULL) == 0 && errno == EDOM);
}

static void test_clock_sleep_deadlines(void) {
  const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_BOOTTIME};
  for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
    struct timespec start = {0, 0}, finish = {0, 0}, request = {0, 5000000};
    int started = clock_gettime(CLOCK_MONOTONIC, &start);
    errno = EDOM;
    int result = ordinary_sleep_call(clocks[i], 0, &request, NULL);
    int saved_errno = errno;
    int ended = clock_gettime(CLOCK_MONOTONIC, &finish);
    check("clock_nanosleep relative request lasts at least five milliseconds",
          started == 0 && ended == 0 && result == 0 &&
          ordinary_elapsed_ns(&start, &finish) >= 5000000);
    check("clock_nanosleep successful wait preserves errno", saved_errno == EDOM);

    struct timespec target = {0, 0}, remain = {111, 222};
    int read_clock = clock_gettime(clocks[i], &target);
    target.tv_nsec += 5000000;
    if (target.tv_nsec >= 1000000000) { ++target.tv_sec; target.tv_nsec -= 1000000000; }
    errno = EDOM;
    result = ordinary_sleep_call(clocks[i], TIMER_ABSTIME, &target, &remain);
    saved_errno = errno;
    ended = clock_gettime(clocks[i], &finish);
    check("clock_nanosleep absolute request reaches its selected clock",
          read_clock == 0 && ended == 0 && result == 0 &&
          ordinary_elapsed_ns(&target, &finish) >= 0);
    check("clock_nanosleep absolute wait preserves errno and remainder",
          saved_errno == EDOM && remain.tv_sec == 111 && remain.tv_nsec == 222);

    struct timespec past = {0, 0};
    remain.tv_sec = 111; remain.tv_nsec = 222; errno = EDOM;
    check("clock_nanosleep past absolute deadline succeeds without changing remainder",
          ordinary_sleep_call(clocks[i], TIMER_ABSTIME, &past, &remain) == 0 &&
          errno == EDOM && remain.tv_sec == 111 && remain.tv_nsec == 222);
  }
}

static volatile sig_atomic_t ordinary_sleep_signal;
static void ordinary_sleep_handler(int signal_number) {
  if (signal_number == SIGUSR1) ordinary_sleep_signal = 1;
}
struct OrdinarySleepSender { pthread_t target; int stop; };
static void* ordinary_sleep_sender(void* value) {
  struct OrdinarySleepSender* sender = (struct OrdinarySleepSender*)value;
  // Repeated bounded attempts avoid a scheduler pause between thread creation
  // and entry into the wait consuming the only signal before the syscall.
  for (int i = 0; i < 20; ++i) {
    usleep(10000);
    if (__atomic_load_n(&sender->stop, __ATOMIC_ACQUIRE)) break;
    pthread_kill(sender->target, SIGUSR1);
  }
  return NULL;
}

static void test_clock_sleep_signals(void) {
  struct sigaction action, previous;
  memset(&action, 0, sizeof(action));
  action.sa_handler = ordinary_sleep_handler;
  sigemptyset(&action.sa_mask);
  int installed = sigaction(SIGUSR1, &action, &previous);
  check("clock_nanosleep signal fixture installs handler", installed == 0);
  if (installed != 0) return;
  sigset_t selected, old_mask;
  sigemptyset(&selected);
  sigaddset(&selected, SIGUSR1);
  int unblocked = pthread_sigmask(SIG_UNBLOCK, &selected, &old_mask);
  check("clock_nanosleep signal fixture unblocks its signal", unblocked == 0);
  if (unblocked != 0) { sigaction(SIGUSR1, &previous, NULL); return; }
  for (int absolute = 0; absolute < 2; ++absolute) {
    struct OrdinarySleepSender sender = {pthread_self(), 0};
    struct timespec request = {2, 0}, remain = {111, 222};
    if (absolute) {
      if (clock_gettime(CLOCK_MONOTONIC, &request) != 0) {
        check("clock_nanosleep interrupt fixture reads clock", 0);
        break;
      }
      request.tv_sec += 2;
    }
    ordinary_sleep_signal = 0;
    pthread_t thread;
    int created = pthread_create(&thread, NULL, ordinary_sleep_sender, &sender);
    check("clock_nanosleep interrupt fixture starts sender", created == 0);
    if (created != 0) continue;
    errno = EDOM;
    int result = ordinary_sleep_call(CLOCK_MONOTONIC, absolute ? TIMER_ABSTIME : 0,
                                    &request, &remain);
    int saved_errno = errno;
    __atomic_store_n(&sender.stop, 1, __ATOMIC_RELEASE);
    int joined = pthread_join(thread, NULL);
    check("clock_nanosleep delivered signal returns EINTR with errno preserved",
          joined == 0 && ordinary_sleep_signal && result == EINTR && saved_errno == EDOM);
    if (absolute) {
      check("clock_nanosleep absolute interruption does not write remainder",
            remain.tv_sec == 111 && remain.tv_nsec == 222);
    } else {
      check("clock_nanosleep relative interruption reports a normalized remaining duration",
            remain.tv_sec >= 0 && remain.tv_sec < 2 &&
            remain.tv_nsec >= 0 && remain.tv_nsec < 1000000000);
    }
  }
  pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
  sigaction(SIGUSR1, &previous, NULL);
}

static void test_affinity_size_contracts(void) {
  cpu_set_t mask, original;
  memset(&mask, 0xa5, sizeof(mask));
  memcpy(&original, &mask, sizeof(mask));
  errno = 0;
  check("sched_getaffinity zero/null returns EINVAL",
        ordinary_affinity_call(0, 0, NULL) == -1 && errno == EINVAL);
  const size_t short_sizes[] = {0, 1, 7};
  for (size_t i = 0; i < sizeof(short_sizes) / sizeof(short_sizes[0]); ++i) {
    errno = 0;
    int result = ordinary_affinity_call(0, short_sizes[i], &mask);
    check("sched_getaffinity rejects a short mask before writing",
          result == -1 && errno == EINVAL && memcmp(&mask, &original, sizeof(mask)) == 0);
  }
  errno = 0;
  check("sched_getaffinity usable-size/null returns EFAULT",
        ordinary_affinity_call(0, sizeof(mask), NULL) == -1 && errno == EFAULT);
  struct { cpu_set_t value; uint64_t after; } guarded;
  memset(&guarded, 0xa5, sizeof(guarded));
  int result = ordinary_affinity_call(0, sizeof(guarded.value), &guarded.value);
  unsigned any = 0;
  const unsigned char* bytes = (const unsigned char*)&guarded.value;
  for (size_t i = 0; i < sizeof(guarded.value); ++i) any |= bytes[i];
  check("sched_getaffinity fills a nonempty mask within the declared capacity",
        result == 0 && any && guarded.after == UINT64_C(0xa5a5a5a5a5a5a5a5));
}

static void test_ordinary_os_contracts(void) {
  test_clock_sleep_inputs();
  test_clock_sleep_deadlines();
  test_clock_sleep_signals();
  test_affinity_size_contracts();
}
