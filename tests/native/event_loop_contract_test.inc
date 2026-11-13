/* Shared ordinary API contracts: Linux reference, Windows POSIX and both guests. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

struct loop_waiter_contract {
  int ep;
  unsigned seen;
  int failed;
};

static void* loop_waiter_contract_run(void* raw) {
  struct loop_waiter_contract* state = (struct loop_waiter_contract*)raw;
  for (int i = 0; i < 2; ++i) {
    struct epoll_event event;
    if (epoll_wait(state->ep, &event, 1, 1000) != 1 || event.data.u32 >= 4) {
      state->failed = 1;
      break;
    }
    state->seen |= 1u << event.data.u32;
  }
  return NULL;
}

static void test_epoll_fairness_contract(void) {
  int ep = epoll_create1(0), items[4] = {-1, -1, -1, -1};
  int setup = ep >= 0;
  for (int i = 0; i < 4; ++i) {
    items[i] = eventfd(1, EFD_NONBLOCK);
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.u32 = (uint32_t)i;
    setup = setup && items[i] >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, items[i], &event) == 0;
  }
  check("epoll fairness: four undrained counters", setup);
  if (!setup) goto cleanup;
  for (int capacity = 1; capacity <= 2; ++capacity) {
    for (int round = 0; round < 3; ++round) {
      unsigned seen = 0;
      int valid = 1;
      for (int wait = 0; wait < 4 / capacity; ++wait) {
        struct epoll_event events[2];
        int count = epoll_wait(ep, events, capacity, 0);
        valid = valid && count == capacity;
        for (int i = 0; i < count; ++i) {
          if (events[i].data.u32 >= 4) valid = 0;
          else seen |= 1u << events[i].data.u32;
        }
      }
      check("epoll fairness: bounded complete ready set", valid && seen == 15);
    }
  }
  {
    int alias = dup(ep);
    unsigned seen = 0;
    int valid = alias >= 0;
    for (int i = 0; i < 4; ++i) {
      struct epoll_event event;
      int count = epoll_wait(i % 2 == 0 ? ep : alias, &event, 1, 0);
      if (count != 1 || event.data.u32 >= 4) valid = 0;
      else seen |= 1u << event.data.u32;
    }
    check("epoll aliases share fairness progress", valid && seen == 15);
    if (alias >= 0) close(alias);
  }
  {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.u32 = 2;
    check("epoll MOD and rearm after earlier delivery", epoll_ctl(ep, EPOLL_CTL_MOD, items[2], &event) == 0);
    unsigned seen = 0;
    for (int i = 0; i < 4; ++i) {
      struct epoll_event out;
      if (epoll_wait(ep, &out, 1, 0) == 1 && out.data.u32 < 4) seen |= 1u << out.data.u32;
    }
    check("epoll oneshot gets its turn", seen == 15);
    int repeated = 0;
    for (int i = 0; i < 4; ++i) {
      struct epoll_event out;
      if (epoll_wait(ep, &out, 1, 0) == 1 && out.data.u32 == 2) repeated = 1;
    }
    check("epoll fairness preserves oneshot retirement", !repeated);
    event.events = EPOLLIN;
    check("epoll restore level-triggered entry", epoll_ctl(ep, EPOLL_CTL_MOD, items[2], &event) == 0);
    check("epoll delete last-delivered candidate", epoll_ctl(ep, EPOLL_CTL_DEL, items[3], NULL) == 0);
    event.data.u32 = 3;
    check("epoll re-add gets a new generation", epoll_ctl(ep, EPOLL_CTL_ADD, items[3], &event) == 0);
  }
  {
    struct loop_waiter_contract a = {ep, 0, 0}, b = {ep, 0, 0};
    pthread_t one, two;
    int created_one = pthread_create(&one, NULL, loop_waiter_contract_run, &a) == 0;
    int created_two = pthread_create(&two, NULL, loop_waiter_contract_run, &b) == 0;
    if (created_one) pthread_join(one, NULL);
    if (created_two) pthread_join(two, NULL);
    check("epoll concurrent waiters share complete rotation", created_one && created_two &&
          !a.failed && !b.failed && (a.seen | b.seen) == 15);
  }
cleanup:
  for (int i = 0; i < 4; ++i) if (items[i] >= 0) close(items[i]);
  if (ep >= 0) close(ep);
}

#if !defined(__ANDROID__) && !defined(__BIONIC__)
/* Android's NDK has no pthread_cancel API. These cancellation cases run in
 * the Linux reference and actual Windows POSIX host; ordinary vector cases
 * below also run unchanged in both Android guest ABIs. */
struct loop_cancel_write_contract {
  int fd;
  int count;
  int entered;
  int returned;
};

static void* loop_cancel_write_contract_run(void* raw) {
  struct loop_cancel_write_contract* state = (struct loop_cancel_write_contract*)raw;
  if (pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL) != 0 ||
      pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0 ||
      pthread_cancel(pthread_self()) != 0) return NULL;
  /* Self-cancellation is pending in deferred mode. Record that execution
   * reached writev, so cancellation during setup cannot satisfy this test. */
  struct iovec empty[2] = {{NULL, 0}, {NULL, 0}};
  state->entered = 1;
  (void)writev(state->fd, empty, state->count);
  state->returned = 1;
  return NULL;
}

static void test_empty_writev_cancellation_contract(void) {
  /* Keep cancellation evidence independent of the uncanceled empty-write
   * cases: a broken older source may already have queued a phantom datagram. */
  int pair[2];
  int opened = socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0;
  check("writev cancellation datagram pair", opened);
  if (!opened) return;
  for (int count = 0; count <= 2; count += 2) {
    struct loop_cancel_write_contract state = {pair[0], count, 0, 0};
    pthread_t worker;
    void* result = NULL;
    int created = pthread_create(&worker, NULL, loop_cancel_write_contract_run, &state) == 0;
    int joined = created && pthread_join(worker, &result) == 0;
    check(count == 0 ? "writev zero-count socket vector delivers pending cancellation"
                     : "writev all-zero socket vector delivers pending cancellation",
          joined && result == PTHREAD_CANCELED && state.entered && !state.returned);
    struct pollfd available = {pair[1], POLLIN, 0};
    check(count == 0 ? "writev canceled zero-count vector creates no datagram"
                     : "writev canceled all-zero vector creates no datagram",
          poll(&available, 1, 0) == 0);
  }
  close(pair[0]); close(pair[1]);
}
#endif

static void test_writev_event_contract(void) {
  int fd = eventfd(0, EFD_NONBLOCK);
  check("writev eventfd creation", fd >= 0);
  if (fd < 0) return;
  uint64_t value = 3, actual = 0;
  struct iovec vector[2] = {{&value, sizeof(value)}, {&value, sizeof(value)}};
  check("writev routes eventfd counter", writev(fd, vector, 1) == 8);
  struct pollfd ready = {fd, POLLIN, 0};
  check("writev makes eventfd readable", poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN));
  check("writev eventfd value retained", read(fd, &actual, sizeof(actual)) == 8 && actual == 3);
  check("writev keeps legacy element boundaries", writev(fd, vector, 2) == 16);
  check("writev eventfd values accumulate", read(fd, &actual, sizeof(actual)) == 8 && actual == 6);
  vector[1].iov_len = 4;
  check("writev retains prefix on short event element", writev(fd, vector, 2) == 8);
  check("writev prefix counter retained", read(fd, &actual, sizeof(actual)) == 8 && actual == 3);
  vector[0].iov_len = 4;
  errno = 0;
  check("writev does not concatenate short event elements", writev(fd, vector, 2) == -1 && errno == EINVAL);
  vector[0].iov_len = vector[1].iov_len = 0;
  check("writev zero-count event vector is a no-op", writev(fd, vector, 0) == 0);
  check("writev all-zero event vector is a no-op", writev(fd, vector, 2) == 0);
  vector[1].iov_len = 8;
  errno = 0;
  check("writev leading empty element retains scalar policy", writev(fd, vector, 2) == -1 && errno == EINVAL);
  vector[0].iov_len = 8;
  {
    int alias = dup(fd);
    check("writev eventfd alias", alias >= 0 && writev(alias, vector, 1) == 8 &&
          read(fd, &actual, sizeof(actual)) == 8 && actual == 3);
    if (alias >= 0) close(alias);
  }
  close(fd);
  for (int kind = 0; kind < 2; ++kind) {
    int readonly = kind ? inotify_init1(IN_NONBLOCK) : epoll_create1(0);
    check("writev readonly event creation", readonly >= 0);
    errno = 0;
    check("writev readonly event rejects data", readonly >= 0 && writev(readonly, vector, 1) == -1 &&
          errno == (kind ? EBADF : EINVAL));
    errno = 0;
    check("writev readonly event rejects empty vector", readonly >= 0 && writev(readonly, vector, 0) == -1 &&
          errno == (kind ? EBADF : EINVAL));
    errno = 0;
    check("scalar readonly event rejects data consistently", readonly >= 0 && write(readonly, &value, sizeof(value)) == -1 &&
          errno == (kind ? EBADF : EINVAL));
    if (readonly >= 0) close(readonly);
  }
  {
    int pair[2];
    char a[] = "ab", b[] = "cde", bytes[6] = {0};
    struct iovec out[2] = {{a, 2}, {b, 3}};
    int opened = socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0;
    check("writev datagram pair", opened);
    if (opened) {
      check("writev socket stays one gather call", writev(pair[0], out, 2) == 5 &&
            read(pair[1], bytes, sizeof(bytes)) == 5 && memcmp(bytes, "abcde", 5) == 0);
      struct iovec empty[2] = {{NULL, 0}, {NULL, 0}};
      check("writev empty socket vectors transfer no data", writev(pair[0], empty, 0) == 0 &&
            writev(pair[0], empty, 2) == 0);
      struct pollfd available = {pair[1], POLLIN, 0};
      check("writev empty socket vectors create no datagram", poll(&available, 1, 0) == 0);
      close(pair[0]); close(pair[1]);
    }
  }
#if !defined(__ANDROID__) && !defined(__BIONIC__)
  test_empty_writev_cancellation_contract();
#endif
  {
    char path[] = "gesher-vector-watch-XXXXXX";
    int file = mkstemp(path), notify = inotify_init1(IN_NONBLOCK);
    int watch = file >= 0 && notify >= 0 ? inotify_add_watch(notify, path, IN_MODIFY) : -1;
    check("writev watched file setup", file >= 0 && notify >= 0 && watch >= 0);
    if (file >= 0 && notify >= 0 && watch >= 0) {
      struct pollfd available = {notify, POLLIN, 0};
      union { uint64_t align; unsigned char bytes[1024]; } events;
      check("writev watched file payload", writev(file, vector, 1) == 8);
      int readable = poll(&available, 1, 1000) == 1 && (available.revents & POLLIN);
      int size = readable ? (int)read(notify, events.bytes, sizeof(events.bytes)) : -1;
      struct inotify_event* event = (struct inotify_event*)events.bytes;
      check("writev file notification arrives before close", size >= (int)sizeof(*event) &&
            event->wd == watch && (event->mask & IN_MODIFY) && fcntl(file, F_GETFD) >= 0);
    }
    if (notify >= 0) close(notify);
    if (file >= 0) { close(file); unlink(path); }
  }
  {
    char path[] = "gesher-vector-access-XXXXXX";
    int file = mkstemp(path), readonly = file >= 0 ? open(path, O_RDONLY) : -1;
    check("writev empty regular file setup", file >= 0 && readonly >= 0);
    if (file >= 0 && readonly >= 0) {
      struct iovec empty[2] = {{NULL, 0}, {NULL, 0}};
      for (int count = 0; count <= 2; count += 2) {
        errno = 0;
        check("writev empty read-only regular file is EBADF", writev(readonly, empty, count) == -1 && errno == EBADF);
        check("writev empty writable regular file is a no-op", writev(file, empty, count) == 0);
      }
      check("writev empty regular file retains position and size", lseek(file, 0, SEEK_CUR) == 0 && lseek(file, 0, SEEK_END) == 0);
    }
    if (readonly >= 0) close(readonly);
    if (file >= 0) { close(file); unlink(path); }
  }
  {
    int pair[2];
    int opened = pipe(pair) == 0;
    check("writev empty pipe setup", opened);
    if (opened) {
      struct iovec empty[2] = {{NULL, 0}, {NULL, 0}};
      for (int count = 0; count <= 2; count += 2) {
        errno = 0;
        check("writev empty pipe read end is EBADF", writev(pair[0], empty, count) == -1 && errno == EBADF);
        check("writev empty pipe write end is a no-op", writev(pair[1], empty, count) == 0);
      }
      struct pollfd available = {pair[0], POLLIN, 0};
      check("writev empty pipe remains unreadable", poll(&available, 1, 0) == 0);
      close(pair[0]); close(pair[1]);
    }
  }
}

static void test_host_select_contract(void) {
  int pair[2];
  int opened = pipe(pair) == 0;
  check("select contract pipe", opened);
  if (!opened) return;
  char value = 'x';
  check("select pipe seed", write(pair[1], &value, 1) == 1);
  fd_set readset;
  FD_ZERO(&readset); FD_SET(pair[0], &readset);
  struct timeval timeout = {1, 0};
  check("select ready descriptor retains residual budget", select(pair[0] + 1, &readset, NULL, NULL, &timeout) == 1 &&
        FD_ISSET(pair[0], &readset) && timeout.tv_sec == 0 && timeout.tv_usec > 0);
  struct timespec ts = {1, 123456};
  FD_ZERO(&readset); FD_SET(pair[0], &readset);
  check("pselect never rewrites caller timespec", pselect(pair[0] + 1, &readset, NULL, NULL, &ts, NULL) == 1 &&
        ts.tv_sec == 1 && ts.tv_nsec == 123456);
  check("select pipe drain", read(pair[0], &value, 1) == 1);
  timeout.tv_sec = 0; timeout.tv_usec = 1000;
  FD_ZERO(&readset); FD_SET(pair[0], &readset);
  check("select timeout clears interests and residual", select(pair[0] + 1, &readset, NULL, NULL, &timeout) == 0 &&
        !FD_ISSET(pair[0], &readset) && timeout.tv_sec == 0 && timeout.tv_usec == 0);
  close(pair[0]); close(pair[1]);
}

static void test_event_loop_contracts(void) {
  test_epoll_fairness_contract();
  test_writev_event_contract();
  test_host_select_contract();
}
