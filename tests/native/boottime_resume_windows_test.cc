// Real Windows power registration, timerfd read/poll/epoll, and guest sleep.
// Default CI mode tests awake operation; --after-suspend requires a recorded
// physical suspend/hibernate run and never suspends the computer itself.
#include <gesher/bionic_shim.h>
#include <gesher/clocks.h>
#include <gesher/resume.h>
#include <gesher/timerfd.h>

#include <windows.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>

#if defined(__x86_64__)
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

namespace {
using Time = gesher::TimerFdTime;
using SleepCall = int(GUEST_ABI*)(int, int, const Time*, Time*);
int assertions, failures;
void Check(const char* name, bool ok) {
  ++assertions;
  if (!ok) ++failures;
  std::printf("boottime-windows: %s %s\n", ok ? "PASS" : "FAIL", name);
}
[[noreturn]] void Stop() {
  std::puts("boottime-windows: FAIL incomplete native wait; terminating test process");
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 1);
  std::_Exit(1);
}
Time Split(int64_t nanos) { return {nanos / 1000000000, nanos % 1000000000}; }
struct ResumeObservation {
  HANDLE event = nullptr;
  std::atomic<int64_t> awake{0}, boot{0};
  static void Arrived(void* context) {
    auto& value = *static_cast<ResumeObservation*>(context);
    value.awake = gesher::MonotonicNanos();
    value.boot = gesher::BoottimeNanos();
    SetEvent(value.event);
  }
};
}

int main(int argc, char** argv) {
  const bool physical = argc == 2 && std::strcmp(argv[1], "--after-suspend") == 0;
  if (argc != 1 && !physical) return 2;
  gesher::bionic::InstallBionicShim();
  auto sleep = reinterpret_cast<SleepCall>(gesher::bionic::LookupBionicSymbol("clock_nanosleep"));
  Check("public guest clock_nanosleep resolves", sleep != nullptr);
  if (sleep == nullptr) return 1;

  const int poll_fd = gesher::TimerFdCreate(7, 04000);
  const int epoll_fd = gesher::TimerFdCreate(7, 04000);
  const int read_fd = gesher::TimerFdCreate(7, 0);
  const int mono_fd = gesher::TimerFdCreate(1, 04000);
  Check("BOOTTIME and MONOTONIC descriptors register", poll_fd >= 0 && epoll_fd >= 0 && read_fd >= 0 && mono_fd >= 0);
  if (poll_fd < 0 || epoll_fd < 0 || read_fd < 0 || mono_fd < 0) return 1;
  const int ep = epoll_create1(0);
  epoll_event interest{}; interest.events = EPOLLIN; interest.data.fd = epoll_fd;
  Check("actual epoll watches BOOTTIME readiness", ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, epoll_fd, &interest) == 0);
  if (ep < 0) return 1;
  const int64_t start_awake = gesher::MonotonicNanos();
  const int64_t start_boot = gesher::BoottimeNanos();
  const int64_t duration = physical ? 60000000000LL : 20000000LL;
  const int64_t deadline = start_boot + duration;
  const int64_t period = physical ? 10000000000LL : 20000000LL;
  const gesher::TimerFdSpec periodic{Split(period), Split(deadline)};
  const gesher::TimerFdSpec one_shot{{0, 0}, Split(deadline)};
  const gesher::TimerFdSpec mono{{0, 0}, Split(start_awake + duration)};
  Check("absolute timer deadlines arm", gesher::TimerFdSetTime(poll_fd, 1, &periodic, nullptr) == 0 &&
        gesher::TimerFdSetTime(epoll_fd, 1, &one_shot, nullptr) == 0 &&
        gesher::TimerFdSetTime(read_fd, 1, &one_shot, nullptr) == 0 &&
        gesher::TimerFdSetTime(mono_fd, 1, &mono, nullptr) == 0);

  auto reader = std::async(std::launch::async, [=] {
    uint64_t count = 0;
    return read(read_fd, &count, sizeof(count)) == sizeof(count) && count == 1;
  });
  auto sleeper = std::async(std::launch::async, [=] {
    const Time until = Split(deadline);
    Time remainder{99, 88}; errno = EDOM;
    return sleep(7, 1, &until, &remainder) == 0 && errno == EDOM &&
           remainder.tv_sec == 99 && remainder.tv_nsec == 88;
  });
  auto poller = std::async(std::launch::async, [=] {
    pollfd descriptor{poll_fd, POLLIN, 0};
    return poll(&descriptor, 1, -1) == 1 && (descriptor.revents & POLLIN) != 0;
  });
  auto epoller = std::async(std::launch::async, [=] {
    epoll_event result{};
    return epoll_wait(ep, &result, 1, -1) == 1 && result.data.fd == epoll_fd &&
           (result.events & EPOLLIN) != 0;
  });

  ResumeObservation observed;
  gesher::ResumeSubscription audit;
  if (physical) {
    observed.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check("physical acceptance observer registers", observed.event != nullptr &&
          audit.Start(ResumeObservation::Arrived, &observed) == 0);
    if (observed.event == nullptr || failures != 0) Stop();
    std::puts("Within 20 seconds, suspend or hibernate Windows for at least 75 seconds, then resume.");
    std::puts("This probe does not request suspend. Record machine, Windows build and actual power mode.");
    std::fflush(stdout);
    if (WaitForSingleObject(observed.event, 180000) != WAIT_OBJECT_0) Stop();
    const int64_t awake_delta = observed.awake - start_awake;
    const int64_t boot_delta = observed.boot - start_boot;
    std::printf("boottime-windows: physical awake=%" PRId64 " boot=%" PRId64 " ns\n", awake_delta, boot_delta);
    Check("recorded run actually crosses deadline only in BOOTTIME", awake_delta < duration && boot_delta >= duration);
    // Timer subscriptions precede the audit observer in the dispatcher.
    // Fresh poll also refreshes an expired clock independently; callback wake
    // evidence comes from the outstanding workers and admitted-wait host tests.
    pollfd ready{poll_fd, POLLIN, 0};
    Check("expired BOOTTIME timer is poll-readable after resume", poll(&ready, 1, 0) == 1 &&
          (ready.revents & POLLIN) != 0);
    pollfd still_waiting{mono_fd, POLLIN, 0};
    Check("MONOTONIC retains its remaining awake timeout", poll(&still_waiting, 1, 0) == 0);
  }

  for (auto* result : {&reader, &sleeper, &poller, &epoller}) {
    if (result->wait_for(std::chrono::seconds(3)) != std::future_status::ready) Stop();
  }
  Check("blocked timer read receives one expiration", reader.get());
  Check("guest BOOTTIME sleep completes without EINTR", sleeper.get());
  Check("blocked native poll wakes", poller.get());
  Check("blocked native epoll wakes", epoller.get());
  const int64_t before = gesher::BoottimeNanos();
  uint64_t count = 0;
  const auto read_count = read(poll_fd, &count, sizeof(count));
  const int64_t after = gesher::BoottimeNanos();
  Check("periodic count includes every elapsed BOOTTIME period", read_count == sizeof(count) &&
        count >= static_cast<uint64_t>((before - deadline) / period + 1) &&
        count <= static_cast<uint64_t>((after - deadline) / period + 1));
  audit.Reset();  // quiesce callback before closing its event/stack context
  if (observed.event != nullptr) CloseHandle(observed.event);
  for (int fd : {ep, poll_fd, epoll_fd, read_fd, mono_fd}) close(fd);
  std::printf("boottime-windows: %d assertions, %d failures; physical=%s\n",
              assertions, failures, physical ? "yes" : "no");
  return failures != 0;
}
