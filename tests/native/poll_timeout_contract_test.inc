// Public LP64 Android wait contracts, reusable in x86-64/ARM64 ELF fixtures.
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct wait_timespec { int64_t tv_sec, tv_nsec; };
struct wait_timeval { int64_t tv_sec, tv_usec; };
struct wait_fd_set { uint64_t bits[16]; };
typedef int (*wait_poll_fn)(struct pollfd*, uint64_t, int);
typedef int (*wait_poll_chk_fn)(struct pollfd*, uint64_t, int, size_t);
typedef int (*wait_ppoll_fn)(struct pollfd*, uint64_t, const struct wait_timespec*, const void*);
typedef int (*wait_ppoll_chk_fn)(struct pollfd*, uint64_t, const struct wait_timespec*, const void*, size_t);
typedef int (*wait_select_fn)(int, struct wait_fd_set*, struct wait_fd_set*, struct wait_fd_set*, struct wait_timeval*);
typedef int (*wait_pselect_fn)(int, struct wait_fd_set*, struct wait_fd_set*, struct wait_fd_set*, const struct wait_timespec*, const void*);
static const char* const wait_ppoll_names[] = {"ppoll", "ppoll64", "__ppoll_chk", "__ppoll64_chk"};
static const char* const wait_pselect_names[] = {"pselect", "pselect64"};
static void wait_set(struct wait_fd_set* set, int fd) {
  set->bits[fd / 64] |= UINT64_C(1) << (fd % 64);
}
static int wait_isset(const struct wait_fd_set* set, int fd) {
  return (set->bits[fd / 64] >> (fd % 64) & 1) != 0;
}
static int wait_call_ppoll(const char* name, struct pollfd* fds, uint64_t count,
                           const struct wait_timespec* timeout, const void* mask) {
  void* symbol = dlsym(RTLD_DEFAULT, name);
  if (symbol == NULL) return -99;
  if (name[0] == '_') {
    return ((wait_ppoll_chk_fn)symbol)(fds, count, timeout, mask, (size_t)count * sizeof(*fds));
  }
  return ((wait_ppoll_fn)symbol)(fds, count, timeout, mask);
}
static int64_t wait_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static void test_poll_timeout_contracts(void) {
  wait_select_fn select_api = (wait_select_fn)dlsym(RTLD_DEFAULT, "select");
  int available = select_api != NULL;
  for (size_t i = 0; i < sizeof(wait_ppoll_names) / sizeof(wait_ppoll_names[0]); ++i)
    available &= dlsym(RTLD_DEFAULT, wait_ppoll_names[i]) != NULL;
  for (size_t i = 0; i < sizeof(wait_pselect_names) / sizeof(wait_pselect_names[0]); ++i)
    available &= dlsym(RTLD_DEFAULT, wait_pselect_names[i]) != NULL;
  available &= dlsym(RTLD_DEFAULT, "poll") != NULL && dlsym(RTLD_DEFAULT, "__poll_chk") != NULL;
  check("poll/select: public exports and aliases", available);
  if (!available) return;
  int fds[2];
  int pipe_ok = pipe(fds) == 0;
  check("poll/select: create readiness pipe", pipe_ok);
  if (!pipe_ok) return;
  if (fds[0] >= 1024) { check("poll/select: pipe fits guest fd_set", 0); close(fds[0]); close(fds[1]); return; }
  check("poll/select: make pipe ready", write(fds[1], "x", 1) == 1);
  const struct wait_timespec invalid[] = {
    {-1, 0}, {INT64_MIN, 0}, {0, -1}, {0, 1000000000}, {0, INT64_MAX}
  };
  for (size_t a = 0; a < sizeof(wait_ppoll_names) / sizeof(wait_ppoll_names[0]); ++a) {
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      struct pollfd ready = {fds[0], POLLIN, 0};
      struct wait_timespec timeout = invalid[i], original = timeout;
      char label[128];
      snprintf(label, sizeof(label), "%s: invalid timespec %zu rejected unchanged", wait_ppoll_names[a], i);
      errno = 0;
      int rc = wait_call_ppoll(wait_ppoll_names[a], &ready, 1, &timeout, NULL);
      check(label, rc == -1 && errno == EINVAL && memcmp(&timeout, &original, sizeof(timeout)) == 0);
    }
    struct pollfd ready = {fds[0], POLLIN, 0};
    struct wait_timespec timeout = {5, 123456789}, original = timeout;
    check("ppoll: ready pipe preserves const timeout",
          wait_call_ppoll(wait_ppoll_names[a], &ready, 1, &timeout, NULL) == 1 &&
          (ready.revents & POLLIN) != 0 && memcmp(&timeout, &original, sizeof(timeout)) == 0);
  }
  for (size_t a = 0; a < sizeof(wait_pselect_names) / sizeof(wait_pselect_names[0]); ++a) {
    wait_pselect_fn pselect_api = (wait_pselect_fn)dlsym(RTLD_DEFAULT, wait_pselect_names[a]);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      struct wait_fd_set read_set = {{0}};
      wait_set(&read_set, fds[0]);
      struct wait_fd_set original_set = read_set;
      struct wait_timespec timeout = invalid[i], original = timeout;
      char label[128];
      snprintf(label, sizeof(label), "%s: invalid timespec %zu preserves inputs", wait_pselect_names[a], i);
      errno = 0;
      int rc = pselect_api(fds[0] + 1, &read_set, NULL, NULL, &timeout, NULL);
      check(label, rc == -1 && errno == EINVAL && memcmp(&read_set, &original_set, sizeof(read_set)) == 0 &&
            memcmp(&timeout, &original, sizeof(timeout)) == 0);
    }
    struct wait_fd_set read_set = {{0}};
    wait_set(&read_set, fds[0]);
    struct wait_timespec timeout = {5, 123456789}, original = timeout;
    check("pselect: ready pipe preserves const timeout",
          pselect_api(fds[0] + 1, &read_set, NULL, NULL, &timeout, NULL) == 1 &&
          wait_isset(&read_set, fds[0]) && memcmp(&timeout, &original, sizeof(timeout)) == 0);
  }
  const struct wait_timeval invalid_tv[] = {
    {-1, 0}, {INT64_MIN, 0}, {0, -1}, {0, 1000000}, {0, INT64_MAX}
  };
  for (size_t i = 0; i < sizeof(invalid_tv) / sizeof(invalid_tv[0]); ++i) {
    struct wait_fd_set read_set = {{0}};
    wait_set(&read_set, fds[0]);
    struct wait_fd_set original_set = read_set;
    struct wait_timeval timeout = invalid_tv[i], original = timeout;
    errno = 0;
    int rc = select_api(fds[0] + 1, &read_set, NULL, NULL, &timeout);
    check("select: invalid timeval preserves input and descriptor sets",
          rc == -1 && errno == EINVAL && memcmp(&read_set, &original_set, sizeof(read_set)) == 0 &&
          memcmp(&timeout, &original, sizeof(timeout)) == 0);
  }
  struct wait_fd_set read_set = {{0}};
  wait_set(&read_set, fds[0]);
  struct wait_timeval timeout = {5, 0};
  int rc = select_api(fds[0] + 1, &read_set, NULL, NULL, &timeout);
  check("select: ready descriptor retains remaining budget", rc == 1 && wait_isset(&read_set, fds[0]) &&
        timeout.tv_sec >= 0 && timeout.tv_sec <= 5 && timeout.tv_usec >= 0 && timeout.tv_usec < 1000000 &&
        (timeout.tv_sec != 0 || timeout.tv_usec != 0));
  for (int nfds = -1; nfds <= 1025; nfds += 1026) {
    struct wait_fd_set before = read_set;
    struct wait_timeval duration = {1, 0};
    errno = 0;
    check("select: invalid nfds does not change inputs", select_api(nfds, &read_set, NULL, NULL, &duration) == -1 &&
          errno == EINVAL && memcmp(&read_set, &before, sizeof(read_set)) == 0 && duration.tv_sec == 1);
  }
  int closed = dup(fds[0]);
  if (closed >= 0 && closed < 1024) {
    close(closed);
    memset(&read_set, 0, sizeof(read_set));
    wait_set(&read_set, fds[0]);
    wait_set(&read_set, closed);
    struct wait_fd_set before = read_set;
    timeout = (struct wait_timeval){1, 0};
    errno = 0;
    rc = select_api((closed > fds[0] ? closed : fds[0]) + 1, &read_set, NULL, NULL, &timeout);
    check("select: invalid descriptor preserves all result sets", rc == -1 && errno == EBADF &&
          memcmp(&read_set, &before, sizeof(read_set)) == 0 && timeout.tv_sec == 1);
  } else {
    if (closed >= 0) close(closed);
    check("poll/select: obtain invalid descriptor witness", 0);
  }
  struct pollfd ready = {fds[0], POLLIN, 0};
  wait_poll_fn poll_api = (wait_poll_fn)dlsym(RTLD_DEFAULT, "poll");
  wait_poll_chk_fn poll_chk = (wait_poll_chk_fn)dlsym(RTLD_DEFAULT, "__poll_chk");
  check("poll: ordinary and checked aliases retain readiness", poll_api(&ready, 1, 0) == 1 &&
        poll_chk(&ready, 1, 0, sizeof(ready)) == 1 && (ready.revents & POLLIN) != 0);
  char byte;
  check("poll/select: drain readiness pipe", read(fds[0], &byte, 1) == 1);
  memset(&read_set, 0, sizeof(read_set));
  wait_set(&read_set, fds[0]);
  timeout = (struct wait_timeval){0, 20000};
  int64_t start = wait_now_ns();
  rc = select_api(fds[0] + 1, &read_set, NULL, NULL, &timeout);
  check("select: finite timeout consumes budget and clears sets", rc == 0 && !wait_isset(&read_set, fds[0]) &&
        timeout.tv_sec == 0 && timeout.tv_usec == 0 && wait_now_ns() - start >= 10000000);
  struct wait_timespec finite = {0, 20000000}, before_finite = finite;
  ready.revents = 0;
  start = wait_now_ns();
  rc = wait_call_ppoll("ppoll", &ready, 1, &finite, NULL);
  check("ppoll: finite timeout leaves const input unchanged", rc == 0 && ready.revents == 0 &&
        memcmp(&finite, &before_finite, sizeof(finite)) == 0 && wait_now_ns() - start >= 10000000);
  close(fds[0]);
  close(fds[1]);
}
