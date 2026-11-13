// gesher-base probe: eventfd and epoll as libutils' Looper uses them, and
// poll() on eventfds, built against gesher-base's Linux headers like AOSP
// code. Prints "all passed" and exits 0 when every check holds.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <windows.h>

#include <atomic>
#include <thread>

#include "../../compat/posix/src/event_internal.h"

static int fails = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      printf("FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno);           \
      ++fails;                                                                \
    }                                                                         \
  } while (0)

static_assert(sizeof(epoll_event) == 12, "Linux's packed x86-64 layout");
static_assert(offsetof(epoll_event, data) == 4, "Linux's packed x86-64 layout");

static int Add(int ep, int fd, uint32_t events, uint64_t tag) {
  epoll_event ev = {};
  ev.events = events;
  ev.data.u64 = tag;
  return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static void check(const char* what, int ok) {
  if (!ok) printf("FAIL nested epoll: %s\n", what);
  CHECK(ok);
}
#include "../../tests/native/epoll_nested_test.inc"
#include "../../tests/native/event_loop_contract_test.inc"

struct PausedSnapshot {
  HANDLE sampled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool resumed = false;
};

static void PauseSnapshot(void* raw) {
  auto& pause = *static_cast<PausedSnapshot*>(raw);
  SetEvent(pause.sampled);
  pause.resumed = WaitForSingleObject(pause.resume, 5000) == WAIT_OBJECT_0;
}

static void TestSnapshotWakeup() {
  int ep = epoll_create1(0), leaf = eventfd(1, EFD_NONBLOCK);
  PausedSnapshot pause;
  CHECK(ep >= 0 && leaf >= 0 && pause.sampled != nullptr && pause.resume != nullptr);
  int result = -2;
  epoll_event stale = {};
  std::thread waiter([&] {
    result = gesher::EpollWaitWithObserver(ep, &stale, 1, 1000, PauseSnapshot, &pause);
  });
  CHECK(WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
  CHECK(Add(ep, leaf, EPOLLIN, 77) == 0);
  // A second waiter consumes any shared auto-reset topology event. The first
  // waiter has not entered poll or registered its generic wake channel yet.
  epoll_event fast = {};
  CHECK(epoll_wait(ep, &fast, 1, 0) == 1 && fast.data.u64 == 77);
  SetEvent(pause.resume);
  waiter.join();
  CHECK(pause.resumed && result == 1 && stale.data.u64 == 77);
  CloseHandle(pause.sampled);
  CloseHandle(pause.resume);
  close(ep);
  close(leaf);
}

static void TestWaiterLifetime() {
  int ep = epoll_create1(0), leaf = eventfd(1, EFD_NONBLOCK);
  CHECK(ep >= 0 && leaf >= 0 && Add(ep, leaf, EPOLLIN, 88) == 0);
  PausedSnapshot pause;
  int result = -2;
  epoll_event out = {};
  std::thread waiter([&] {
    result = gesher::EpollWaitWithObserver(ep, &out, 1, 1000, PauseSnapshot, &pause);
  });
  CHECK(WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
  CHECK(close(ep) == 0);
  int replacement = epoll_create1(0);
  SetEvent(pause.resume);
  waiter.join();
  CHECK(pause.resumed && result == 1 && out.data.u64 == 88);
  CloseHandle(pause.sampled);
  CloseHandle(pause.resume);
  close(leaf);
  // The retained old object's waiter must not become attached to a new epoll
  // if its CRT descriptor number is reused. Returning/timeout closes channels.
  CHECK(replacement >= 0 && epoll_wait(replacement, &out, 1, 1) == 0);
  DWORD before = 0, after = 0;
  CHECK(GetProcessHandleCount(GetCurrentProcess(), &before) != 0);
  for (int i = 0; i < 128; ++i) CHECK(epoll_wait(replacement, &out, 1, 0) == 0);
  CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after == before);
  close(replacement);
}

// Writes `value` to the eventfd `fd` after `delay_ms`, from a pthread.
struct DelayedWrite {
  int fd;
  DWORD delay_ms;
  uint64_t value;
};

static void* WriteLater(void* arg) {
  auto* w = static_cast<DelayedWrite*>(arg);
  Sleep(w->delay_ms);
  write(w->fd, &w->value, sizeof(w->value));
  return nullptr;
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_epoll_nested();
  test_event_loop_contracts();
  TestSnapshotWakeup();
  TestWaiterLifetime();
  uint64_t v = 0;

  // eventfd: a counter that reads return and reset.
  int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  CHECK(efd >= 0);
  CHECK(read(efd, &v, sizeof(v)) == -1 && errno == EAGAIN);
  CHECK((fcntl(efd, F_GETFL) & O_NONBLOCK) != 0);
  v = 3;
  CHECK(write(efd, &v, sizeof(v)) == 8);
  v = 4;
  CHECK(write(efd, &v, sizeof(v)) == 8);
  uint32_t small = 0;
  CHECK(read(efd, &small, sizeof(small)) == -1 && errno == EINVAL);
  CHECK(write(efd, &small, sizeof(small)) == -1 && errno == EINVAL);
  v = 0;
  CHECK(read(efd, &v, sizeof(v)) == 8 && v == 7);
  CHECK(read(efd, &v, sizeof(v)) == -1 && errno == EAGAIN);
  v = ~uint64_t{0};
  CHECK(write(efd, &v, sizeof(v)) == -1 && errno == EINVAL);
  // The counter holds at most 2^64 - 2; past it a non-blocking write fails.
  v = ~uint64_t{0} - 1;
  CHECK(write(efd, &v, sizeof(v)) == 8);
  v = 1;
  CHECK(write(efd, &v, sizeof(v)) == -1 && errno == EAGAIN);
  eventfd_t ev_value = 0;
  CHECK(eventfd_read(efd, &ev_value) == 0 && ev_value == ~uint64_t{0} - 1);
  CHECK(eventfd_write(efd, 5) == 0 && eventfd_read(efd, &ev_value) == 0 && ev_value == 5);
  CHECK(eventfd(0, 0x40) == -1 && errno == EINVAL);

  // Semaphore mode: each read takes one.
  int sem = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
  CHECK(sem >= 0);
  CHECK(read(sem, &v, sizeof(v)) == 8 && v == 1);
  CHECK(read(sem, &v, sizeof(v)) == 8 && v == 1);
  CHECK(read(sem, &v, sizeof(v)) == -1 && errno == EAGAIN);
  CHECK(close(sem) == 0);

  // A blocking read waits for a write from another thread.
  int blocking = eventfd(0, 0);
  CHECK(blocking >= 0 && (fcntl(blocking, F_GETFL) & O_NONBLOCK) == 0);
  {
    std::thread writer([&] {
      Sleep(100);
      eventfd_write(blocking, 9);
    });
    DWORD t0 = GetTickCount();
    CHECK(read(blocking, &v, sizeof(v)) == 8 && v == 9);
    CHECK(GetTickCount() - t0 >= 80);
    writer.join();
  }
  // fcntl makes it non-blocking.
  CHECK(fcntl(blocking, F_SETFL, O_NONBLOCK) == 0);
  CHECK(read(blocking, &v, sizeof(v)) == -1 && errno == EAGAIN);

  // dup and dup2 share the counter, as they share the file on Linux.
  int d = dup(efd);
  CHECK(d >= 0 && d != efd);
  CHECK(eventfd_write(d, 2) == 0 && eventfd_read(efd, &ev_value) == 0 && ev_value == 2);
  CHECK(dup2(efd, blocking) == blocking);
  CHECK(eventfd_write(efd, 6) == 0 && eventfd_read(blocking, &ev_value) == 0 && ev_value == 6);
  CHECK(close(d) == 0 && close(blocking) == 0);
  CHECK(read(blocking, &v, sizeof(v)) == -1 && errno == EBADF);

  // poll on an eventfd: writable, then readable once written; woken at once
  // by another thread's write.
  {
    pollfd p = {efd, POLLIN | POLLOUT, 0};
    CHECK(poll(&p, 1, 0) == 1 && p.revents == POLLOUT);
    p.events = POLLIN;
    DWORD t0 = GetTickCount();
    CHECK(poll(&p, 1, 50) == 0 && p.revents == 0);
    CHECK(GetTickCount() - t0 >= 40);
    DelayedWrite w = {efd, 100, 1};
    pthread_t writer;
    CHECK(pthread_create(&writer, nullptr, WriteLater, &w) == 0);
    t0 = GetTickCount();
    CHECK(poll(&p, 1, 5000) == 1 && p.revents == POLLIN);
    CHECK(GetTickCount() - t0 < 500);
    pthread_join(writer, nullptr);
    CHECK(eventfd_read(efd, &ev_value) == 0 && ev_value == 1);
  }

  // epoll_ctl's errors.
  int ep = epoll_create1(EPOLL_CLOEXEC);
  CHECK(ep >= 0);
  CHECK(epoll_create1(1) == -1 && errno == EINVAL);
  CHECK(epoll_create(0) == -1 && errno == EINVAL);
  int ep2 = epoll_create(1);
  CHECK(ep2 >= 0 && close(ep2) == 0);
  CHECK(Add(ep, efd, EPOLLIN, 1) == 0);
  CHECK(Add(ep, efd, EPOLLIN, 1) == -1 && errno == EEXIST);
  epoll_event ev = {};
  ev.events = EPOLLIN;
  int other = eventfd(0, EFD_NONBLOCK);
  CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, other, &ev) == -1 && errno == ENOENT);
  CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, other, nullptr) == -1 && errno == ENOENT);
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, 12345, &ev) == -1 && errno == EBADF);
  CHECK(epoll_ctl(12345, EPOLL_CTL_ADD, efd, &ev) == -1 && errno == EBADF);
  CHECK(epoll_ctl(other, EPOLL_CTL_ADD, efd, &ev) == -1 && errno == EINVAL);  // not an epoll
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) == -1 && errno == EINVAL);
  CHECK(epoll_ctl(ep, 99, other, &ev) == -1 && errno == EINVAL);
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, other, nullptr) == -1 && errno == EFAULT);
  char file_name[MAX_PATH];
  snprintf(file_name, sizeof(file_name), "epoll_probe_%lu.tmp", GetCurrentProcessId());
  int file = open(file_name, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(file >= 0);
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, file, &ev) == -1 && errno == EPERM);  // as Linux
  close(file);
  unlink(file_name);
  epoll_event out[8] = {};
  CHECK(epoll_wait(ep, out, 0, 0) == -1 && errno == EINVAL);
  CHECK(epoll_wait(other, out, 8, 0) == -1 && errno == EINVAL);
  CHECK(epoll_wait(12345, out, 8, 0) == -1 && errno == EBADF);
  CHECK(read(ep, &v, sizeof(v)) == -1 && errno == EINVAL);

  // epoll_wait times out, then reports the eventfd with its data.
  DWORD t0 = GetTickCount();
  CHECK(epoll_wait(ep, out, 8, 100) == 0);
  CHECK(GetTickCount() - t0 >= 90);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);
  CHECK(eventfd_write(efd, 1) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].events == EPOLLIN && out[0].data.u64 == 1);
  // Level-triggered: still readable, still reported.
  CHECK(epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 1);
  CHECK(eventfd_read(efd, &ev_value) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);

  // Looper's wake: a write from another pthread ends a 5 s wait at once.
  {
    DelayedWrite w = {efd, 100, 1};
    pthread_t writer;
    CHECK(pthread_create(&writer, nullptr, WriteLater, &w) == 0);
    t0 = GetTickCount();
    CHECK(epoll_wait(ep, out, 8, 5000) == 1 && out[0].data.u64 == 1);
    DWORD took = GetTickCount() - t0;
    CHECK(took >= 80 && took < 500);
    pthread_join(writer, nullptr);
    CHECK(eventfd_read(efd, &ev_value) == 0);
  }

  // MOD changes the data and events; EPOLLOUT is always ready on an eventfd.
  ev.events = EPOLLIN | EPOLLOUT;
  ev.data.u64 = 2;
  CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].events == EPOLLOUT && out[0].data.u64 == 2);
  ev.events = EPOLLIN;
  CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev) == 0);

  // A socket pair becoming readable, alongside the eventfd; and the eventfd
  // still wakes the wait at once.
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  CHECK(Add(ep, sv[1], EPOLLIN, 3) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);
  CHECK(write(sv[0], "x", 1) == 1);
  CHECK(epoll_wait(ep, out, 8, 1000) == 1 && out[0].events == EPOLLIN && out[0].data.u64 == 3);
  char c;
  CHECK(read(sv[1], &c, 1) == 1);
  {
    DelayedWrite w = {efd, 100, 1};
    pthread_t writer;
    CHECK(pthread_create(&writer, nullptr, WriteLater, &w) == 0);
    t0 = GetTickCount();
    CHECK(epoll_wait(ep, out, 8, 5000) == 1 && out[0].data.u64 == 2);
    CHECK(GetTickCount() - t0 < 500);
    pthread_join(writer, nullptr);
    CHECK(eventfd_read(efd, &ev_value) == 0);
  }
  // The peer closing: readable with a hangup.
  CHECK(close(sv[0]) == 0);
  CHECK(epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 3 &&
        (out[0].events & EPOLLIN) != 0);
  CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, sv[1], nullptr) == 0);
  CHECK(close(sv[1]) == 0);

  // A pipe becoming readable.
  int pp[2];
  CHECK(pipe(pp) == 0);
  CHECK(Add(ep, pp[0], EPOLLIN, 4) == 0);
  CHECK(epoll_wait(ep, out, 8, 30) == 0);
  CHECK(write(pp[1], "y", 1) == 1);
  CHECK(epoll_wait(ep, out, 8, 1000) == 1 && out[0].events == EPOLLIN && out[0].data.u64 == 4);
  CHECK(read(pp[0], &c, 1) == 1);
  CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, pp[0], nullptr) == 0);
  close(pp[0]);
  close(pp[1]);

  // EPOLLONESHOT: reported once, until MOD arms it again.
  CHECK(Add(ep, other, EPOLLIN | EPOLLONESHOT, 5) == 0);
  CHECK(eventfd_write(other, 1) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 5);
  CHECK(epoll_wait(ep, out, 8, 50) == 0);
  ev.events = EPOLLIN | EPOLLONESHOT;
  ev.data.u64 = 6;
  CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, other, &ev) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 6);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);

  // maxevents caps what one wait returns.
  ev.events = EPOLLIN;
  CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, other, &ev) == 0);
  CHECK(eventfd_write(efd, 1) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 2);
  CHECK(epoll_wait(ep, out, 1, 0) == 1);
  CHECK(eventfd_read(efd, &ev_value) == 0);

  // DEL, and close, take an fd out of the list; a new fd that reuses the
  // number is not reported for the old entry.
  CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, other, nullptr) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);
  CHECK(Add(ep, other, EPOLLIN, 7) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 7);
  CHECK(close(other) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);
  int reused = eventfd(1, EFD_NONBLOCK);
  CHECK(reused >= 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 0);
  CHECK(Add(ep, reused, EPOLLIN, 8) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 8);
  CHECK(close(reused) == 0);

  // An fd added while another thread waits wakes that wait.
  {
    int late = eventfd(1, EFD_NONBLOCK);
    std::atomic<int> got{-2};
    std::atomic<uint64_t> tag{0};
    std::atomic<DWORD> took{0};
    std::thread waiter([&] {
      epoll_event e[4];
      DWORD start = GetTickCount();
      got = epoll_wait(ep, e, 4, 5000);
      took = GetTickCount() - start;
      tag = e[0].data.u64;
    });
    Sleep(100);
    CHECK(Add(ep, late, EPOLLIN, 9) == 0);
    waiter.join();
    CHECK(got == 1 && tag == 9 && took < 500);
    close(late);
  }

  // A pipe written by another thread wakes the wait at once, as Dart's event
  // handler needs: it waits on its interrupt pipe and a timerfd, with or
  // without sockets, and every socket read hands its interest back through
  // that pipe. 200 round trips took 5 s when the wait saw the pipe only on
  // its next 10 ms look.
  for (int with_socket = 0; with_socket < 2; ++with_socket) {
    int ping[2], pong[2], idle[2] = {-1, -1};
    CHECK(pipe(ping) == 0 && pipe(pong) == 0);
    int wait_ep = epoll_create1(0);
    int timer = eventfd(0, EFD_NONBLOCK);
    CHECK(Add(wait_ep, ping[0], EPOLLIN, 1) == 0 && Add(wait_ep, timer, EPOLLIN, 2) == 0);
    if (with_socket) {
      CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, idle) == 0);
      CHECK(Add(wait_ep, idle[0], EPOLLIN | EPOLLET, 3) == 0);
    }
    constexpr int kRounds = 200;
    std::atomic<int> answered{0};
    std::thread echo([&] {
      epoll_event e[4];
      char b;
      for (int i = 0; i < kRounds; ++i) {
        if (epoll_wait(wait_ep, e, 4, 5000) != 1 || e[0].data.u64 != 1) break;
        if (read(ping[0], &b, 1) != 1 || write(pong[1], &b, 1) != 1) break;
        ++answered;
      }
    });
    pollfd back = {pong[0], POLLIN, 0};
    DWORD start = GetTickCount();
    for (int i = 0; i < kRounds; ++i) {
      char b = 'p';
      if (write(ping[1], &b, 1) != 1 || poll(&back, 1, 5000) != 1 || read(pong[0], &b, 1) != 1) break;
    }
    DWORD took = GetTickCount() - start;
    echo.join();
    printf("pipe round trips through epoll%s: %d in %lu ms\n", with_socket ? " beside a socket" : "",
           answered.load(), static_cast<unsigned long>(took));
    CHECK(answered == kRounds && took < 1000);
    for (int fd : {ping[0], ping[1], pong[0], pong[1], timer, wait_ep, idle[0], idle[1]}) {
      if (fd >= 0) close(fd);
    }
  }

  CHECK(close(ep) == 0);
  CHECK(epoll_wait(ep, out, 8, 0) == -1 && errno == EBADF);
  CHECK(close(efd) == 0);

  printf(fails != 0 ? "%d failures\n" : "all passed\n", fails);
  return fails != 0;
}
