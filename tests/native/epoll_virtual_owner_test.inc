// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Public calls shared by native Linux, the production epoll host and Windows.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int owner_ctl(int ep, int op, int fd, uint32_t events, uint64_t data) {
  struct epoll_event event = {};
  event.events = events;
  event.data.u64 = data;
  return epoll_ctl(ep, op, fd, &event);
}

static int owner_events(int ep, uint64_t first, uint64_t second = 0, int timeout = 0) {
  struct epoll_event events[4] = {};
  int count = epoll_wait(ep, events, 4, timeout);
  int expected = first == 0 ? 0 : second == 0 ? 1 : 2;
  if (count != expected) return 0;
  unsigned seen = 0;
  for (int i = 0; i < count; ++i) {
    if (events[i].events != EPOLLIN) return 0;
    unsigned bit = events[i].data.u64 == first ? 1 :
                   second != 0 && events[i].data.u64 == second ? 2 : 0;
    if (bit == 0 || (seen & bit) != 0) return 0;
    seen |= bit;
  }
  return seen == (expected == 0 ? 0u : expected == 1 ? 1u : 3u);
}

static int owner_place(int source, int target) {
  if (source == target) return target;
  int result = dup2(source, target);
  close(source);
  return result;
}

static void test_epoll_virtual_owners() {
  const uint32_t modes[] = {EPOLLIN, EPOLLIN | EPOLLET,
      EPOLLIN | EPOLLONESHOT, EPOLLIN | EPOLLET | EPOLLONESHOT};
  for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int original = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int alias = dup(original), extra = fcntl(original, F_DUPFD_CLOEXEC, 0);
    check("owner setup", ep >= 0 && original >= 0 && alias >= 0 && extra >= 0);
    check("owner two alias registrations", owner_ctl(ep, EPOLL_CTL_ADD, original, modes[mode], 11) == 0 &&
          owner_ctl(ep, EPOLL_CTL_ADD, alias, modes[mode], 12) == 0);
    check("owner same key duplicate is EEXIST", owner_ctl(ep, EPOLL_CTL_ADD, original, EPOLLIN, 13) == -1 && errno == EEXIST);
    check("owner close watched number", close(original) == 0);
    check("owner surviving alias writes", eventfd_write(extra, 3) == 0);
    check("owner both independent registrations survive", owner_events(ep, 11, 12));
    check("owner level and one-shot/edge modes preserved", modes[mode] == EPOLLIN ? owner_events(ep, 11, 12) : owner_events(ep, 0));
    eventfd_t value = 0;
    check("owner alias drains shared counter", eventfd_read(alias, &value) == 0 && value == 3 && owner_events(ep, 0));
    check("owner closed number control is EBADF", owner_ctl(ep, EPOLL_CTL_MOD, original, EPOLLIN, 14) == -1 && errno == EBADF);
    check("owner alias-specific removal", owner_ctl(ep, EPOLL_CTL_DEL, alias, 0, 0) == 0);
    check("owner different alias cannot remove original key", owner_ctl(ep, EPOLL_CTL_DEL, extra, 0, 0) == -1 && errno == ENOENT);
    check("owner replacement uses exact watched number", owner_place(eventfd(0, EFD_NONBLOCK), original) == original);
    check("owner reused number cannot modify old file", owner_ctl(ep, EPOLL_CTL_MOD, original, EPOLLIN, 15) == -1 && errno == ENOENT);
    check("owner reused number cannot remove old file", owner_ctl(ep, EPOLL_CTL_DEL, original, 0, 0) == -1 && errno == ENOENT);
    check("owner reused number adds independent file", owner_ctl(ep, EPOLL_CTL_ADD, original, EPOLLIN, 16) == 0);
    check("owner replacement publication is isolated", eventfd_write(original, 1) == 0 && owner_events(ep, 16));
    check("owner replacement drain", eventfd_read(original, &value) == 0 && value == 1);
    check("owner old source publication remains isolated", eventfd_write(alias, 2) == 0 &&
          ((modes[mode] & EPOLLONESHOT) != 0 ? owner_events(ep, 0) : owner_events(ep, 11)));
    check("owner old source drain", eventfd_read(extra, &value) == 0 && value == 2);
    int restored = mode & 1 ? dup3(alias, original, O_CLOEXEC) : dup2(alias, original);
    check("owner original number restored to same file", restored == original);
    check("owner restored identity can rearm original key", owner_ctl(ep, EPOLL_CTL_MOD, original, modes[mode], 17) == 0);
    check("owner other aliases close without retirement", close(alias) == 0 && close(extra) == 0);
    check("owner restored registration receives", eventfd_write(original, 9) == 0 && owner_events(ep, 17));
    check("owner final alias retires both generations", close(original) == 0 && owner_events(ep, 0));
    check("owner epoll cleanup", close(ep) == 0);
  }

  int parent = epoll_create1(0), child = epoll_create1(0);
  int child_alias = fcntl(child, F_DUPFD, 0), input = eventfd(0, EFD_NONBLOCK);
  check("owner nested setup", parent >= 0 && child >= 0 && child_alias >= 0 && input >= 0 &&
        owner_ctl(child, EPOLL_CTL_ADD, input, EPOLLIN, 21) == 0 &&
        owner_ctl(parent, EPOLL_CTL_ADD, child, EPOLLIN, 22) == 0);
  check("owner nested original closes", close(child) == 0);
  check("owner nested readiness follows owner", eventfd_write(input, 1) == 0 && owner_events(parent, 22));
  check("owner nested poll does not consume child", owner_events(child_alias, 21) && owner_events(parent, 22));
  check("owner nesting cycle survives alias close", owner_ctl(child_alias, EPOLL_CTL_ADD, parent, EPOLLIN, 23) == -1 && errno == ELOOP);
  eventfd_t value = 0;
  check("owner nested drain", eventfd_read(input, &value) == 0 && value == 1 && owner_events(parent, 0));
  check("owner nested numeric reuse", owner_place(epoll_create1(0), child) == child);
  int second_input = eventfd(0, EFD_NONBLOCK);
  check("owner nested replacement registration", second_input >= 0 &&
        owner_ctl(child, EPOLL_CTL_ADD, second_input, EPOLLIN, 24) == 0 &&
        owner_ctl(parent, EPOLL_CTL_ADD, child, EPOLLIN, 25) == 0);
  check("owner two nested files coexist", eventfd_write(input, 1) == 0 && eventfd_write(second_input, 1) == 0 && owner_events(parent, 22, 25));
  check("owner nested replaced file retires independently", dup2(child_alias, child) == child && owner_events(parent, 22));
  check("owner epoll self-alias is EINVAL", owner_ctl(child_alias, EPOLL_CTL_ADD, child, EPOLLIN, 26) == -1 && errno == EINVAL);
  check("owner restored nested identity can delete", owner_ctl(parent, EPOLL_CTL_DEL, child, 0, 0) == 0 && owner_events(parent, 0));
  close(child_alias); close(child); close(input); close(second_input); close(parent);
  printf("virtual epoll owner contracts: complete\n");
}

#ifdef OWNER_READONLY_REAL
#include <stdlib.h>
#include <sys/inotify.h>
#ifdef _WIN32
#include <gesher/timerfd.h>
#else
#include <sys/timerfd.h>
#endif
#include <time.h>

static void test_epoll_readonly_owners() {
  int ep = epoll_create1(0);
#ifdef _WIN32
  int timer = gesher::TimerFdCreate(1, 04000 | 02000000);
#else
  int timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
#endif
  int alias = dup(timer);
  check("owner actual timer setup", ep >= 0 && timer >= 0 && alias >= 0 && owner_ctl(ep, EPOLL_CTL_ADD, timer, EPOLLIN, 31) == 0);
  check("owner actual timer original closes", close(timer) == 0);
#ifdef _WIN32
  gesher::TimerFdSpec spec{};
#else
  struct itimerspec spec = {};
#endif
  spec.it_value.tv_nsec = 1000000;
#ifdef _WIN32
  int armed = gesher::TimerFdSetTime(alias, 0, &spec, nullptr);
#else
  int armed = timerfd_settime(alias, 0, &spec, nullptr);
#endif
  check("owner actual timer publishes through alias", armed == 0 && owner_events(ep, 31, 0, 2000));
  uint64_t ticks = 0;
  check("owner actual timer read", read(alias, &ticks, sizeof(ticks)) == sizeof(ticks) && ticks == 1);
  check("owner actual timer final close", close(alias) == 0 && owner_events(ep, 0));
  close(ep);

  char path[] = "gesher-epoll-owner-XXXXXX";
  int file = mkstemp(path), notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  alias = dup(notify); ep = epoll_create1(0);
  check("owner actual inotify setup", file >= 0 && notify >= 0 && alias >= 0 && ep >= 0 &&
        inotify_add_watch(alias, path, IN_MODIFY) >= 0 && owner_ctl(ep, EPOLL_CTL_ADD, notify, EPOLLIN, 32) == 0);
  check("owner actual inotify original closes", close(notify) == 0);
  check("owner actual inotify publication", write(file, "x", 1) == 1 && owner_events(ep, 32, 0, 2000));
  char events[1024];
  check("owner actual inotify read", read(alias, events, sizeof(events)) >= (int)sizeof(struct inotify_event));
  check("owner actual inotify final close", close(alias) == 0 && owner_events(ep, 0));
  close(file); unlink(path); close(ep);
  printf("readonly epoll owner contracts: complete\n");
}
#endif
