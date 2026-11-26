// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Shared public epoll corpus: real Linux syscalls and real gesher-base Windows
// timer/inotify producers. The host fixture declares its controlled clocks,
// threadpool and raw inotify record boundary before including this corpus.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(_WIN32) || defined(GESHER_READONLY_EDGES)
#include <gesher/timerfd.h>
#else
#include <sys/timerfd.h>
#endif

static int readonly_timer() {
#if defined(_WIN32) || defined(GESHER_READONLY_EDGES)
  return gesher::TimerFdCreate(1, 04000 | 02000000);
#else
  return timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
#endif
}
static bool readonly_arm(int fd, int first_ms, int period_ms = 0) {
#if defined(_WIN32) || defined(GESHER_READONLY_EDGES)
  gesher::TimerFdSpec spec{{period_ms / 1000, (period_ms % 1000) * 1000000},
                          {first_ms / 1000, (first_ms % 1000) * 1000000}};
  return gesher::TimerFdSetTime(fd, 0, &spec, nullptr) == 0;
#else
  itimerspec spec{{period_ms / 1000, (period_ms % 1000) * 1000000},
                  {first_ms / 1000, (first_ms % 1000) * 1000000}};
  return timerfd_settime(fd, 0, &spec, nullptr) == 0;
#endif
}
static bool readonly_get(int fd) {
#if defined(_WIN32) || defined(GESHER_READONLY_EDGES)
  gesher::TimerFdSpec spec{};
  return gesher::TimerFdGetTime(fd, &spec) == 0 &&
         (spec.it_value.tv_sec > 0 || spec.it_value.tv_nsec > 0);
#else
  itimerspec spec{};
  return timerfd_gettime(fd, &spec) == 0 &&
         (spec.it_value.tv_sec > 0 || spec.it_value.tv_nsec > 0);
#endif
}
#ifndef GESHER_READONLY_EDGES
static void readonly_advance(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
struct ReadonlyInotify {
  int fd = -1;
  int observer = -1;
  std::string directory;
  std::vector<std::string> paths;
  ReadonlyInotify() {
    char name[] = "gesher-epoll-readonly-XXXXXX";
    if (!mkdtemp(name)) return;
    directory = name;
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, directory.c_str(), IN_CREATE) < 0) {
      close(fd); fd = -1;
    }
    if (fd >= 0) {
      observer = epoll_create1(EPOLL_CLOEXEC);
      epoll_event event{};
      event.events = EPOLLIN | EPOLLET;
      if (observer < 0 || epoll_ctl(observer, EPOLL_CTL_ADD, fd, &event) != 0) {
        if (observer >= 0) close(observer);
        observer = -1;
      }
    }
  }
  bool Emit(unsigned id) {
    paths.push_back(directory + "/event-" + std::to_string(id));
    int file = open(paths.back().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0 || close(file) != 0 || observer < 0) return false;
    // Win32 directory completion is asynchronous. An independent registration
    // observes this publication before the tested entry's MOD/drain operations;
    // it neither drains records nor consumes the tested entry's notification.
    epoll_event event{};
    return epoll_wait(observer, &event, 1, 1000) == 1 && event.events == EPOLLIN;
  }
  ~ReadonlyInotify() {
    if (fd >= 0) close(fd);
    if (observer >= 0) close(observer);
    for (const auto& path : paths) unlink(path.c_str());
    if (!directory.empty()) rmdir(directory.c_str());
  }
};
#endif

static bool readonly_ctl(int ep, int op, int fd, uint32_t events, uint64_t data) {
  epoll_event event{};
  event.events = events; event.data.u64 = data;
  return epoll_ctl(ep, op, fd, &event) == 0;
}
static bool readonly_event(int ep, uint64_t data, uint32_t events = EPOLLIN) {
  epoll_event event{};
  return epoll_wait(ep, &event, 1, 1000) == 1 &&
         event.data.u64 == data && event.events == events;
}
static bool readonly_empty(int ep, int timeout = 0) {
  epoll_event event{};
  return epoll_wait(ep, &event, 1, timeout) == 0;
}
static int readonly_drain(int fd, bool timer) {
  alignas(inotify_event) char buffer[4096];
  int total = 0, got;
  do {
    got = static_cast<int>(read(fd, buffer, timer ? 8 : sizeof(buffer)));
    if (got > 0) total += got;
  } while (got > 0);
  return errno == EAGAIN ? total : -1;
}

static void readonly_source_contracts(bool timer) {
  // Always clean both native source kinds, including setup failures.
  ReadonlyInotify inotify;
  int fd = timer ? readonly_timer() : inotify.fd;
  int ep = epoll_create1(EPOLL_CLOEXEC);
  check("readonly source and epoll creation", fd >= 0 && ep >= 0);
  if (fd < 0 || ep < 0) {
    if (ep >= 0) close(ep);
    if (timer && fd >= 0) close(fd);
    return;
  }
  unsigned serial = 0;
  auto publish = [&] {
    if (!timer) return inotify.Emit(++serial);
    bool armed = readonly_arm(fd, 1);
    readonly_advance(5);
    return armed;
  };
  check("readonly ADD edge", readonly_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 11));
  check("readonly initially empty", readonly_empty(ep));
  check("readonly first producer", publish());
  check("readonly first edge", readonly_event(ep, 11));
  int repeated = 0;
  for (int i = 0; i < 10000; ++i) {
    epoll_event event{};
    repeated += epoll_wait(ep, &event, 1, 0) > 0;
  }
  std::printf("readonly work %s: idle_waits=10000 repeated_deliveries=%d\n",
              timer ? "timerfd" : "inotify", repeated);
  check("readonly persistent level does not republish an edge", repeated == 0);
  pollfd level{fd, POLLIN, 0};
  check("readonly level poll remains ready", poll(&level, 1, 0) == 1 && level.revents == POLLIN);
  check("readonly consumed edge waits for its timeout", readonly_empty(ep, 15));
  check("readonly new producer while unread", publish());
  check("readonly new unread notification", readonly_event(ep, 11));
  check("readonly notification consumed once", readonly_empty(ep));

  // Initial ADD/MOD readiness is per registration, independent of generations.
  check("readonly MOD already readable", readonly_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET, 12));
  check("readonly MOD samples current level", readonly_event(ep, 12));
  check("readonly MOD edge consumed", readonly_empty(ep));
  check("readonly drain", readonly_drain(fd, timer) > 0);
  check("readonly drain-refill between waits", publish());
  check("readonly drain-refill edge retained", readonly_event(ep, 12));

  const uint32_t mask = EPOLLIN | EPOLLRDNORM;
  check("readonly input band selection", readonly_ctl(ep, EPOLL_CTL_MOD, fd, mask | EPOLLET, 13));
  check("readonly input band result", readonly_event(ep, 13, timer ? uint32_t{EPOLLIN} : mask));
  check("readonly writable interest", readonly_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLOUT | EPOLLWRNORM | EPOLLET, 14));
  check("readonly never writable", readonly_empty(ep, 5));
  check("readonly normal-only interest", readonly_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLRDNORM | EPOLLET, 15));
  check("readonly normal-only result", timer ? readonly_empty(ep) : readonly_event(ep, 15, EPOLLRDNORM));

  check("readonly ONESHOT MOD", readonly_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET | EPOLLONESHOT, 16));
  check("readonly ONESHOT first", readonly_event(ep, 16));
  check("readonly ONESHOT producer", publish());
  check("readonly ONESHOT remains disarmed", readonly_empty(ep, 5));
  check("readonly ONESHOT rearm", readonly_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET | EPOLLONESHOT, 17));
  check("readonly ONESHOT rearmed level", readonly_event(ep, 17));

  // Duplicated descriptors share producer state but have separate epoll entries.
  int alias = dup(fd), second = epoll_create1(EPOLL_CLOEXEC);
  check("readonly alias and second epoll", alias >= 0 && second >= 0);
  if (alias >= 0 && second >= 0) {
    check("readonly first registration rearm", readonly_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET, 18));
    check("readonly alias registration", readonly_ctl(ep, EPOLL_CTL_ADD, alias, EPOLLIN | EPOLLET, 19));
    check("readonly second epoll registration", readonly_ctl(second, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 20));
    epoll_event events[2]{};
    int count = epoll_wait(ep, events, 2, 1000);
    check("readonly both alias entries retain independent edges", count == 2 &&
          ((events[0].data.u64 == 18 && events[1].data.u64 == 19) ||
           (events[0].data.u64 == 19 && events[1].data.u64 == 18)));
    check("readonly independent epoll delivery", readonly_event(second, 20));
    check("readonly alias drains shared source", readonly_drain(alias, timer) > 0);
    check("readonly DEL alias before close", epoll_ctl(ep, EPOLL_CTL_DEL, alias, nullptr) == 0);
    check("readonly alias close", close(alias) == 0); alias = -1;
    check("readonly surviving source publishes", publish());
    check("readonly surviving registration", readonly_event(ep, 18));
    check("readonly surviving second epoll", readonly_event(second, 20));
    check("readonly remaining source cleanup", readonly_drain(fd, timer) > 0);
  }
  if (alias >= 0) close(alias);
  if (second >= 0) close(second);

  check("readonly DEL", epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr) == 0);
  check("readonly detached source publication", publish());
  check("readonly deleted interest remains empty", readonly_empty(ep));
  check("readonly ADD current readiness", readonly_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET | EPOLLONESHOT, 21));
  pollfd nested{ep, POLLIN, 0};
  check("readonly nested poll samples", poll(&nested, 1, 0) == 1 && nested.revents == POLLIN);
  check("readonly repeated nested poll does not consume", poll(&nested, 1, 0) == 1 && nested.revents == POLLIN);
  check("readonly nested ONESHOT still deliverable", readonly_event(ep, 21));
  check("readonly nested poll after delivery", poll(&nested, 1, 0) == 0);
  close(ep);
  if (timer) close(fd);
}

static void readonly_periodic_contracts() {
  int fd = readonly_timer(), ep = epoll_create1(EPOLL_CLOEXEC);
  check("periodic setup", fd >= 0 && ep >= 0);
  if (fd < 0 || ep < 0) { if (fd >= 0) close(fd); if (ep >= 0) close(ep); return; }
  check("periodic edge registration", readonly_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 31));
  check("periodic arm", readonly_arm(fd, 10, 10));
  readonly_advance(35);
  check("periodic first expiry", readonly_event(ep, 31));
  readonly_advance(40);
  check("periodic unread ticks are not new edges", readonly_empty(ep));
  check("periodic gettime forwards/rearms without draining", readonly_get(fd));
  check("periodic gettime itself is not a notification", readonly_empty(ep));
  readonly_advance(25);
  check("periodic expiry after gettime is a new unread edge", readonly_event(ep, 31));
  uint64_t ticks = 0;
  check("periodic deferred ticks retained", read(fd, &ticks, sizeof(ticks)) == sizeof(ticks) && ticks >= 10);
  check("periodic drained edge", readonly_empty(ep));
  readonly_advance(25);
  check("periodic read rearms next expiry", readonly_event(ep, 31));
  check("periodic disarm clears pending", readonly_arm(fd, 0));
  readonly_advance(25);
  check("periodic disarmed stays empty", readonly_empty(ep));
  check("periodic disarmed read EAGAIN", read(fd, &ticks, sizeof(ticks)) == -1 && errno == EAGAIN);
  close(ep); close(fd);
}

#if !defined(_WIN32) && !defined(GESHER_READONLY_EDGES)
static void readonly_linux_overflow() {
  unsigned limit = 0;
  FILE* setting = fopen("/proc/sys/fs/inotify/max_queued_events", "r");
  if (setting) { if (fscanf(setting, "%u", &limit) != 1) limit = 0; fclose(setting); }
  check("Linux bounded inotify queue configuration", limit >= 2 && limit <= 65536);
  if (limit < 2 || limit > 65536) return;
  char filler_path[] = "gesher-epoll-fill-XXXXXX", modified_path[] = "gesher-epoll-modify-XXXXXX";
  int filler = mkstemp(filler_path), modified = mkstemp(modified_path);
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC), ep = epoll_create1(EPOLL_CLOEXEC);
  int watch = fd >= 0 && modified >= 0 ? inotify_add_watch(fd, modified_path, IN_MODIFY) : -1;
  check("Linux overflow real producer setup", filler >= 0 && modified >= 0 && fd >= 0 && ep >= 0 && watch >= 0);
  if (filler >= 0 && modified >= 0 && fd >= 0 && ep >= 0 && watch >= 0) {
    auto fill = [&](unsigned count) {
      for (unsigned i = 0; i < count; ++i) {
        // Synchronous IN_IGNORED records have distinct watch IDs and no names;
        // two persistent files suffice even for a full 16,384-record queue.
        int wd = inotify_add_watch(fd, filler_path, IN_ATTRIB);
        if (wd < 0 || inotify_rm_watch(fd, wd) != 0) return false;
      }
      return true;
    };
    check("Linux overflow edge registration", readonly_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 71));
    check("Linux overflow fills all but final slot", fill(limit - 1));
    check("Linux overflow tail modification", write(modified, "a", 1) == 1);
    check("Linux overflow full normal queue edge", readonly_event(ep, 71));
    check("Linux full-tail duplicate write", write(modified, "b", 1) == 1);
    check("Linux full-tail duplicate publishes overflow before merge", readonly_event(ep, 71));
    check("Linux repeated overflow write", write(modified, "c", 1) == 1);
    check("Linux queued overflow suppresses new edge", readonly_empty(ep));
    inotify_event partial[2];
    check("Linux overflow partial read", read(fd, partial, sizeof(partial)) == sizeof(partial));
    check("Linux regular refill after retained loss marker", fill(1));
    check("Linux regular refill publishes", readonly_event(ep, 71));
    check("Linux another overflow write with non-tail marker", write(modified, "d", 1) == 1);
    check("Linux non-tail overflow suppresses new edge", readonly_empty(ep));
    std::vector<inotify_event> through_marker(limit - 1);
    const size_t bytes = through_marker.size() * sizeof(inotify_event);
    check("Linux consumes old overflow while retaining regular tail", read(fd, through_marker.data(), bytes) == static_cast<ssize_t>(bytes));
    unsigned losses = 0;
    for (const auto& event : through_marker) losses += event.mask == IN_Q_OVERFLOW;
    check("Linux retains exactly one canonical loss marker", losses == 1 && through_marker.back().wd == -1 &&
          through_marker.back().mask == IN_Q_OVERFLOW && through_marker.back().len == 0);
    check("Linux refill after loss consumption", fill(limit - 1));
    check("Linux refilled queue notification", readonly_event(ep, 71));
    check("Linux later overflow write", write(modified, "e", 1) == 1);
    check("Linux consumed loss marker permits new overflow", readonly_event(ep, 71));
    std::vector<inotify_event> remaining(limit + 1);
    const size_t remaining_bytes = remaining.size() * sizeof(inotify_event);
    check("Linux later queue remains bounded", read(fd, remaining.data(), remaining_bytes) == static_cast<ssize_t>(remaining_bytes));
    losses = 0;
    for (const auto& event : remaining) losses += event.mask == IN_Q_OVERFLOW;
    check("Linux later queue has one new loss marker", losses == 1);
    std::printf("readonly Linux overflow: capacity=%u retained_markers=%u\n", limit, losses);
  }
  if (ep >= 0) close(ep);
  if (fd >= 0) close(fd);
  if (filler >= 0) close(filler);
  if (modified >= 0) close(modified);
  unlink(filler_path); unlink(modified_path);
}
#endif

static void test_epoll_readonly_edges() {
  readonly_source_contracts(true);
  readonly_source_contracts(false);
  readonly_periodic_contracts();
#if !defined(_WIN32) && !defined(GESHER_READONLY_EDGES)
  readonly_linux_overflow();
#endif
  std::puts("readonly edge contracts: complete");
}
