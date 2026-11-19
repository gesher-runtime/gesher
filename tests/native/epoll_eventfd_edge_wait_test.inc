// Real Win32 production-path cases in epoll_probe. The host-source fixture uses
// explicitly declared Win32/clock/descriptor boundaries for these same cases.
struct EdgePausedSnapshot {
  HANDLE sampled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool resumed = false;
  ~EdgePausedSnapshot() { CloseHandle(sampled); CloseHandle(resume); }
};
static void EdgePauseSnapshot(void* raw) {
  auto& pause = *static_cast<EdgePausedSnapshot*>(raw);
  SetEvent(pause.sampled);
  pause.resumed = WaitForSingleObject(pause.resume, 5000) == WAIT_OBJECT_0;
}
static uint64_t EdgeCpuTime() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return UINT64_MAX;
  return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
         (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
}
static void test_epoll_eventfd_edge_waits() {
  int ep = epoll_create1(0), fd = eventfd(1, EFD_NONBLOCK);
  check("eventfd native wait setup", ep >= 0 && fd >= 0 &&
        edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 81) == 0 && edge_wait(ep, EPOLLIN, 81, 0));
  if (ep < 0 || fd < 0) { if (ep >= 0) close(ep); if (fd >= 0) close(fd); return; }
  {
    EdgePausedSnapshot pause;
    int result = -2; epoll_event event = {};
    std::thread waiter([&] { result = gesher::EpollWaitWithObserver(ep, &event, 1, 500, EdgePauseSnapshot, &pause); });
    check("eventfd snapshot reached", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
    check("eventfd producer in snapshot-to-wait gap", eventfd_write(fd, 1) == 0);
    SetEvent(pause.resume); waiter.join();
    check("eventfd gap notification remains pending", pause.resumed && result == 1 && event.events == EPOLLIN && event.data.u64 == 81);
    check("eventfd gap edge consumed once", edge_empty(ep, 0));
  }
  {
    EdgePausedSnapshot one, two;
    int a = -2, b = -2; epoll_event first = {}, second = {};
    std::thread wa([&] { a = gesher::EpollWaitWithObserver(ep, &first, 1, 150, EdgePauseSnapshot, &one); });
    std::thread wb([&] { b = gesher::EpollWaitWithObserver(ep, &second, 1, 150, EdgePauseSnapshot, &two); });
    check("eventfd both waiter snapshots reached", WaitForSingleObject(one.sampled, 5000) == WAIT_OBJECT_0 &&
          WaitForSingleObject(two.sampled, 5000) == WAIT_OBJECT_0);
    check("eventfd one producer for two waiters", eventfd_write(fd, 1) == 0);
    SetEvent(one.resume); SetEvent(two.resume); wa.join(); wb.join();
    check("eventfd concurrent waiters consume one queued edge", one.resumed && two.resumed &&
          ((a == 1 && b == 0 && first.data.u64 == 81) || (b == 1 && a == 0 && second.data.u64 == 81)));
  }
  {
    uint64_t cpu_before = EdgeCpuTime(); int64_t start = edge_millis();
    bool empty = edge_empty(ep, 250);
    int64_t elapsed = edge_millis() - start; uint64_t cpu_after = EdgeCpuTime();
    check("eventfd suppressed level waits for deadline", empty && elapsed >= 200);
    check("eventfd suppressed level does not spin", cpu_before != UINT64_MAX && cpu_after != UINT64_MAX &&
          cpu_after >= cpu_before && cpu_after - cpu_before < 1000000);  // <100 ms CPU
  }
  {
    DWORD before = 0, after = 0;
    check("eventfd native handle baseline", GetProcessHandleCount(GetCurrentProcess(), &before) != 0);
    bool repeated = true;
    for (int i = 0; i < 128; ++i) repeated &= edge_empty(ep, 0);
    check("eventfd repeated suppressed waits", repeated);
    check("eventfd waiter handles reclaimed", GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && before == after);
    // Replacement retires the old weak subscription; subsequent writes must
    // remain bounded and use only the latest cookie and one-shot state.
    bool replaced = true;
    for (int i = 0; i < 128; ++i) {
      replaced &= edge_ctl(ep, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLET, 82) == 0 && edge_wait(ep, EPOLLIN, 82, 0);
    }
    check("eventfd repeated subscription replacement", replaced);
    check("eventfd replacement final publication", eventfd_write(fd, 1) == 0 && edge_wait(ep, EPOLLIN, 82, 0) && edge_empty(ep, 0));
  }
  {
    EdgePausedSnapshot pause;
    int result = -2; epoll_event event = {};
    std::thread waiter([&] { result = gesher::EpollWaitWithObserver(ep, &event, 1, 150, EdgePauseSnapshot, &pause); });
    check("eventfd deletion snapshot reached", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
    check("eventfd deletion while waiting", epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr) == 0 && eventfd_write(fd, 1) == 0);
    SetEvent(pause.resume); waiter.join();
    check("eventfd removed subscription never delivers", pause.resumed && result == 0);
  }
  check("eventfd re-add before close", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 83) == 0 && edge_wait(ep, EPOLLIN, 83, 0));
  {
    EdgePausedSnapshot pause;
    int result = -2; epoll_event event = {};
    std::thread waiter([&] { result = gesher::EpollWaitWithObserver(ep, &event, 1, 150, EdgePauseSnapshot, &pause); });
    check("eventfd close snapshot reached", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
    check("eventfd close source", close(fd) == 0);
    fd = eventfd(1, EFD_NONBLOCK);
    SetEvent(pause.resume); waiter.join();
    check("eventfd reused source cannot satisfy old snapshot", pause.resumed && result == 0 && edge_empty(ep, 0));
    check("eventfd reused source registers independently", edge_ctl(ep, EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLET, 84) == 0 && edge_wait(ep, EPOLLIN, 84, 0));
  }
  {
    EdgePausedSnapshot pause;
    int result = -2; epoll_event event = {};
    std::thread waiter([&] { result = gesher::EpollWaitWithObserver(ep, &event, 1, 500, EdgePauseSnapshot, &pause); });
    check("eventfd epoll-close snapshot reached", WaitForSingleObject(pause.sampled, 5000) == WAIT_OBJECT_0);
    check("eventfd close epoll retains active wait", close(ep) == 0);
    ep = epoll_create1(0);
    check("eventfd producer after epoll close", eventfd_write(fd, 1) == 0);
    SetEvent(pause.resume); waiter.join();
    check("eventfd retained waiter channel survives epoll close", pause.resumed && result == 1 && event.data.u64 == 84);
    check("eventfd replacement epoll stays empty", edge_empty(ep, 0));
  }
  close(ep); close(fd);
  printf("eventfd edge wait contracts: complete\n");
}
