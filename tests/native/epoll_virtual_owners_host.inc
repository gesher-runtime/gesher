// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Appended to the complete epoll.cc host fixture. Only the descriptor syscalls,
// wait boundary and Win32 events are adapters; registry/owners/ctl/read/wait are
// the production implementation. Linux and Windows also run the public corpus.

// Defined before the fixture's syscall macros by the Python source assembler.
static int adapted_dup2(int source, int target) {
  gesher::record_locks::DescriptorGuard descriptors;
  int result = HostDup2(source, target);
  if (result >= 0 && source != target) {
    // Exact virtual-registry retirement order in fd.cc::DupTo, after its CRT
    // duplicate succeeds. Other file/device/socket metadata is outside scope.
    gesher::ForgetEventObject(target);
    gesher::ForgetEpollEntries(target);
    gesher::DupEventObject(source, target);
  }
  return result;
}
static int adapted_dup3(int source, int target, int flags) {
  if (source == target || (flags & ~O_CLOEXEC) != 0) return errno = EINVAL, -1;
  return adapted_dup2(source, target);
}
static int adapted_fcntl(int fd, int command, int argument) {
  gesher::record_locks::DescriptorGuard descriptors;
  int result = HostFcntl(fd, command, argument);
  if (result >= 0 && (command == F_DUPFD || command == F_DUPFD_CLOEXEC))
    gesher::DupEventObject(fd, result);
  return result;
}
#define dup2 adapted_dup2
#define dup3 adapted_dup3
#define fcntl adapted_fcntl
#include "epoll_virtual_owner_test.inc"

static bool owner_spin_until(const std::function<bool()>& ready) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do { if (ready()) return true; std::this_thread::yield(); }
  while (std::chrono::steady_clock::now() < end);
  return false;
}

static void test_owner_wait_lifetimes() {
  for (int mode = 0; mode < 2; ++mode) {
    int ep = epoll_create1(0), fd = eventfd(0, EFD_NONBLOCK);
    auto source = gesher::LookupEventFd(fd);
    std::weak_ptr<gesher::EventFd> weak = source;
    source.reset();
    check("owner weak waiter setup", owner_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN, 41) == 0);
    EdgePausedSnapshot pause;
    int result = -2; epoll_event event = {};
    std::thread waiter([&] {
      result = gesher::EpollWaitWithObserver(ep, &event, 1, 150, EdgePauseSnapshot, &pause);
    });
    check("owner weak snapshot reached", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
    if (mode != 0) {
      SetEvent(pause.resume);
      check("owner actual wait boundary reached", owner_spin_until([] { return host_blocked_polls.load() != 0; }));
      owner_poll_return_reached = false;
      owner_allow_poll_return = false;
      owner_poll_return_resumed = false;
      owner_pause_poll_return = true;
    }
    check("owner last close during epoll observation", close(fd) == 0);
    if (mode != 0)
      check("owner poll wake retains its captured boundary references", owner_spin_until([] { return owner_poll_return_reached.load(); }));
    check("owner snapshot/wait never pins final file", weak.expired());
    owner_allow_poll_return = true;
    SetEvent(pause.resume); waiter.join();
    check("owner retired entry cannot deliver", pause.resumed && result == 0 && owner_events(ep, 0));
    if (mode != 0) check("owner poll return barrier released", owner_poll_return_resumed);
    close(ep);
  }

  // A genuine blocked read, unlike an epoll observation, owns a file reference.
  // The isolated source publication below releases that actual ReadEventObject;
  // it does not pretend an application can write an already closed fd number.
  int ep = epoll_create1(0), fd = eventfd(0, 0);
  std::weak_ptr<gesher::EventFd> weak = gesher::LookupEventFd(fd);
  check("owner in-flight read setup", owner_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN, 42) == 0);
  int result = -2; uint64_t value = 0;
  std::thread reader([&] { gesher::ReadEventObject(fd, &value, sizeof(value), &result); });
  check("owner actual read captured file", owner_spin_until([&] {
    auto observed = weak.lock(); return observed && observed.use_count() >= 3;
  }));
  check("owner close while real read is active", close(fd) == 0 && !weak.expired());
  auto retained = weak.lock();
  check("owner real read preserves registration", retained && !gesher::LookupEpoll(ep)->entries.empty());
  uint64_t one = 1;
  check("owner release actual blocked read", retained && gesher::WriteEventFd(*retained, &one, sizeof(one)) == sizeof(one));
  retained.reset(); reader.join();
  check("owner actual read result", result == sizeof(value) && value == 1);
  check("owner final real I/O retirement", weak.expired() && owner_events(ep, 0));
  close(ep);

  // A nested target may have no public aliases while its own already-admitted
  // epoll_wait still owns it. The outer observation must not become another
  // persistent owner when that real operation returns.
  int parent = epoll_create1(0), child = epoll_create1(0), input = eventfd(0, EFD_NONBLOCK);
  std::weak_ptr<gesher::Epoll> nested = gesher::LookupEpoll(child);
  check("owner aliasless nested setup", owner_ctl(child, EPOLL_CTL_ADD, input, EPOLLIN, 43) == 0 &&
        owner_ctl(parent, EPOLL_CTL_ADD, child, EPOLLIN, 44) == 0);
  EdgePausedSnapshot pause;
  int child_result = -2; epoll_event child_event = {};
  std::thread child_waiter([&] {
    child_result = gesher::EpollWaitWithObserver(child, &child_event, 1, 500, EdgePauseSnapshot, &pause);
  });
  check("owner admitted nested wait reached snapshot", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
  check("owner nested final fd close preserves active operation", close(child) == 0 && !nested.expired());
  check("owner nested source publishes without numeric target", eventfd_write(input, 1) == 0 && owner_events(parent, 44, 0, 500));
  SetEvent(pause.resume); child_waiter.join();
  check("owner admitted nested wait returns actual result", pause.resumed && child_result == 1 && child_event.data.u64 == 43);
  check("owner nested observation cannot keep target alive", nested.expired() && owner_events(parent, 0));
  close(input); close(parent);
}

static void test_owner_snapshot_reuse() {
  for (int iteration = 0; iteration < 32; ++iteration) {
    int ep = epoll_create1(0), watched = eventfd(0, EFD_NONBLOCK), alias = dup(watched);
    check("owner reuse snapshot setup", owner_ctl(ep, EPOLL_CTL_ADD, watched, EPOLLIN | EPOLLET, 51) == 0);
    EdgePausedSnapshot pause;
    int result = -2; epoll_event event = {};
    std::thread waiter([&] {
      result = gesher::EpollWaitWithObserver(ep, &event, 1, 500, EdgePauseSnapshot, &pause);
    });
    check("owner reuse snapshot reached", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
    check("owner close and reuse behind paused snapshot", close(watched) == 0 && owner_place(eventfd(1, EFD_NONBLOCK), watched) == watched);
    check("owner captured source publication", eventfd_write(alias, 1) == 0);
    SetEvent(pause.resume); waiter.join();
    check("owner captured description survives numeric reuse", pause.resumed && result == 1 && event.events == EPOLLIN && event.data.u64 == 51);
    check("owner replacement cannot impersonate old source", owner_ctl(ep, EPOLL_CTL_MOD, watched, EPOLLIN, 52) == -1 && errno == ENOENT);
    eventfd_t value = 0;
    check("owner captured and replacement counters separate", eventfd_read(alias, &value) == 0 && value == 1 &&
          eventfd_read(watched, &value) == 0 && value == 1);
    close(alias); close(watched); close(ep);
  }
}

static void measure_owner_close_work() {
  constexpr int epoll_total = 256, closes = 4096;
  std::vector<int> epolls, sources;
  bool setup = true;
  for (int i = 0; i < epoll_total; ++i) { int fd = epoll_create1(0); epolls.push_back(fd); setup &= fd >= 0; }
  for (int i = 0; i < closes; ++i) { int fd = eventfd(0, EFD_NONBLOCK); sources.push_back(fd); setup &= fd >= 0; }
  check("owner close-work resource setup", setup);
  owner_allocations = 0; owner_allocation_bytes = 0;
  auto start = std::chrono::steady_clock::now();
  owner_count_allocations = true;
  for (int fd : sources) close(fd);
  owner_count_allocations = false;
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  printf("owner close work: epolls=%d closes=%d allocations=%zu bytes=%zu elapsed_ns=%lld\n",
         epoll_total, closes, owner_allocations, owner_allocation_bytes, static_cast<long long>(elapsed));
  check("owner unrelated virtual close does not allocate global census", owner_allocations == 0);
  for (int fd : epolls) close(fd);
}

static void measure_owner_common_work() {
  constexpr int count = 64, waits = 10000;
  int ep = epoll_create1(0);
  std::vector<int> sources;
  sources.reserve(count);
  owner_allocations = 0; owner_allocation_bytes = 0; owner_count_allocations = true;
  for (int i = 0; i < count; ++i) sources.push_back(eventfd(1, EFD_NONBLOCK));
  owner_count_allocations = false;
  printf("owner creation work: files=%d allocations=%zu bytes=%zu (includes declared native-event adapters)\n",
         count, owner_allocations, owner_allocation_bytes);
  owner_allocations = 0; owner_allocation_bytes = 0; owner_count_allocations = true;
  for (int i = 0; i < count; ++i) owner_ctl(ep, EPOLL_CTL_ADD, sources[i], EPOLLIN, i + 1);
  owner_count_allocations = false;
  printf("owner registration work: registrations=%d allocations=%zu bytes=%zu\n", count, owner_allocations, owner_allocation_bytes);
  epoll_event events[count];
  check("owner common wait warmup", epoll_wait(ep, events, count, 0) == count);
  owner_allocations = 0; owner_allocation_bytes = 0;
  auto start = std::chrono::steady_clock::now();
  owner_count_allocations = true;
  bool complete = true;
  for (int i = 0; i < waits; ++i) complete &= epoll_wait(ep, events, count, 0) == count;
  owner_count_allocations = false;
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  check("owner common wait count", complete);
  printf("owner ready wait work: registrations=%d waits=%d allocations=%zu bytes=%zu elapsed_ns=%lld (includes native-event adapters)\n",
         count, waits, owner_allocations, owner_allocation_bytes, static_cast<long long>(elapsed));
  for (int fd : sources) close(fd);
  close(ep);
}

template <typename Payload, typename Register, typename Lookup>
static void test_readonly_payload_owner(Register register_payload, Lookup lookup, int& destroyed) {
  const int before = destroyed;
  int ep = epoll_create1(0);
  auto payload = std::make_shared<Payload>();
  payload->ready = gesher::NewReadiness(true);
  int fd = register_payload(payload, payload->ready), alias = dup(fd);
  payload.reset();
  std::weak_ptr<Payload> weak = lookup(fd);
  check("owner readonly payload registered", fd >= 0 && alias >= 0 && owner_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN, 61) == 0);
  check("owner readonly original close retains alias", close(fd) == 0 && !weak.expired() && destroyed == before);
  auto active = lookup(alias);  // a genuine payload operation may retain ownership
  check("owner readonly operation aliases same file", active && !weak.owner_before(active) && !active.owner_before(weak));
  gesher::SetReadable(*active->ready, true);
  check("owner readonly source published after original close", owner_events(ep, 61));
  check("owner readonly numeric replacement isolated", owner_place(eventfd(1, EFD_NONBLOCK), fd) == fd &&
        owner_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN, 62) == 0 && owner_events(ep, 61, 62));
  check("owner readonly last alias does not revoke real operation", close(alias) == 0 && !weak.expired() && destroyed == before && owner_events(ep, 61, 62));
  active.reset();
  check("owner readonly last real reference retires only its registration", weak.expired() && destroyed == before + 1 && owner_events(ep, 62));
  close(fd); close(ep);
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--measure") == 0) {
    measure_owner_common_work(); measure_owner_close_work(); return failed != 0;
  }
  if (argc == 2 && strcmp(argv[1], "--wait-lifetime-only") == 0) {
    test_owner_wait_lifetimes();
    printf("virtual owner wait lifetime control: %d checks, %d failures\n", checked, failed);
    return failed != 0;
  }
  if (argc != 1) return 2;
  existing_edge_main();
  test_epoll_virtual_owners();
  test_owner_wait_lifetimes();
  test_owner_snapshot_reuse();
  test_readonly_payload_owner<gesher::TimerFd>(gesher::RegisterTimerFd, gesher::LookupTimerFd, gesher::owner_timer_destroyed);
  test_readonly_payload_owner<gesher::Inotify>(gesher::RegisterInotify, gesher::LookupInotify, gesher::owner_inotify_destroyed);
  measure_owner_close_work();
  measure_owner_common_work();
  check("owner all fd metadata retired", gesher::object_count.load() == 0);
  check("owner all native boundary handles retired", host_handles == 0);
  printf("virtual epoll owner production host: %d checks, %d failures\n", checked, failed);
  return failed != 0;
}
