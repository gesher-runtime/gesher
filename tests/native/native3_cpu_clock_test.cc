// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "base/time_utils.h"

#include <windows.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <thread>
#include <time.h>

namespace {
int failures = 0;

void Check(bool ok, const char* name) {
  std::printf("%s  native3 clocks: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}

uint64_t FileTimeValue(const FILETIME& time) {
  return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
}

uint64_t NativeCpuTime(bool process) {
  FILETIME creation, exit, kernel, user;
  BOOL ok = process ? GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)
                    : GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
  return ok ? (FileTimeValue(kernel) + FileTimeValue(user)) * 100 : UINT64_MAX;
}

bool MatchesNativeCpu(bool process) {
  uint64_t before = NativeCpuTime(process);
  uint64_t actual = process ? art::ProcessCpuNanoTime() : art::ThreadCpuNanoTime();
  uint64_t after = NativeCpuTime(process);
  if (before == UINT64_MAX || actual == UINT64_MAX || after == UINT64_MAX ||
      actual < before || actual > after) {
    std::printf("INFO  native3 clocks: %s native bracket %" PRIu64 " <= ART %" PRIu64
                " <= %" PRIu64 " ns\n", process ? "process" : "thread", before, actual, after);
    return false;
  }
  return true;
}

uint64_t NativeMonotonic(uint64_t scale) {
  LARGE_INTEGER frequency, counter;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
      !QueryPerformanceCounter(&counter) || counter.QuadPart < 0) {
    return UINT64_MAX;
  }
  return static_cast<uint64_t>(static_cast<unsigned __int128>(counter.QuadPart) * scale /
                               static_cast<uint64_t>(frequency.QuadPart));
}

bool MatchesNativeMonotonic(uint64_t (*clock)(), uint64_t scale) {
  uint64_t before = NativeMonotonic(scale);
  uint64_t actual = clock();
  uint64_t after = NativeMonotonic(scale);
  if (before == UINT64_MAX || after == UINT64_MAX || actual < before || actual > after) {
    std::printf("INFO  native3 clocks: scale %" PRIu64 " QPC bracket %" PRIu64
                " <= ART %" PRIu64 " <= %" PRIu64 "\n", scale, before, actual, after);
    return false;
  }
  return true;
}

bool MonotonicSequence() {
  uint64_t nano = art::NanoTime(), micro = art::MicroTime(), milli = art::MilliTime();
  for (unsigned i = 0; i < 256; ++i) {
    uint64_t next_nano = art::NanoTime(), next_micro = art::MicroTime(), next_milli = art::MilliTime();
    if (next_nano < nano || next_micro < micro || next_milli < milli) return false;
    nano = next_nano;
    micro = next_micro;
    milli = next_milli;
  }
  return MatchesNativeMonotonic(art::NanoTime, UINT64_C(1000000000)) &&
         MatchesNativeMonotonic(art::MicroTime, UINT64_C(1000000)) &&
         MatchesNativeMonotonic(art::MilliTime, UINT64_C(1000));
}

bool RejectsWallClock() {
  uint64_t before = NativeMonotonic(UINT64_C(1000000));
  FILETIME time;
  GetSystemTimePreciseAsFileTime(&time);
  uint64_t wall = (FileTimeValue(time) - UINT64_C(116444736000000000)) / 10;
  uint64_t after = NativeMonotonic(UINT64_C(1000000));
  return before != UINT64_MAX && after != UINT64_MAX && (wall < before || wall > after);
}

uint64_t NativeClockNs(int clock) {
  if (clock == CLOCK_MONOTONIC) return NativeMonotonic(UINT64_C(1000000000));
  FILETIME time;
  GetSystemTimePreciseAsFileTime(&time);
  return (FileTimeValue(time) - UINT64_C(116444736000000000)) * 100;
}

bool MatchesDeadline(int clock) {
  constexpr uint64_t delay = UINT64_C(1234567890);
  uint64_t before = NativeClockNs(clock);
  timespec deadline;
  art::InitTimeSpec(true, clock, 1234, 567890, &deadline);
  uint64_t after = NativeClockNs(clock);
  if (before == UINT64_MAX || after == UINT64_MAX || deadline.tv_sec < 0 ||
      deadline.tv_nsec < 0 || deadline.tv_nsec >= 1000000000L) return false;
  uint64_t actual = static_cast<uint64_t>(deadline.tv_sec) * UINT64_C(1000000000) + deadline.tv_nsec;
  if (actual < before + delay || actual > after + delay) {
    std::printf("INFO  native3 clocks: deadline clock %d bracket %" PRIu64
                " <= ART %" PRIu64 " <= %" PRIu64 " ns\n", clock, before + delay,
                actual, after + delay);
    return false;
  }
  return true;
}
}  // namespace

int main() {
  Check(MatchesNativeMonotonic(art::NanoTime, UINT64_C(1000000000)),
        "ART nanoseconds match the independent QPC bracket");
  Check(MatchesNativeMonotonic(art::MicroTime, UINT64_C(1000000)),
        "ART microseconds match the independent QPC bracket");
  Check(MatchesNativeMonotonic(art::MilliTime, UINT64_C(1000)),
        "ART milliseconds match the independent QPC bracket");
  Check(RejectsWallClock(), "the monotonic bracket rejects the old wall-clock domain");
  Check(MonotonicSequence(), "all ART monotonic clocks keep their order and units");
  uint64_t nano_before = art::NanoTime();
  Sleep(20);
  uint64_t nano_after = art::NanoTime();
  Check(nano_after >= nano_before && nano_after - nano_before >= UINT64_C(1000000) &&
            MatchesNativeMonotonic(art::NanoTime, UINT64_C(1000000000)),
        "the ART elapsed clock advances across Sleep");
  bool other_monotonic = false;
  std::thread other([&] { other_monotonic = MonotonicSequence(); });
  bool caller_monotonic = MonotonicSequence();
  other.join();
  Check(caller_monotonic && other_monotonic, "concurrent callers share the QPC clock domain");
  Check(MatchesDeadline(CLOCK_MONOTONIC),
        "absolute monotonic deadlines use QPC and normalize their offset");
  Check(MatchesDeadline(CLOCK_REALTIME),
        "absolute realtime deadlines use UTC and normalize their offset");
  timespec relative;
  art::InitTimeSpec(false, CLOCK_MONOTONIC, 1234, 567890, &relative);
  bool relative_ok = relative.tv_sec == 1 && relative.tv_nsec == 234567890;
  art::InitTimeSpec(false, CLOCK_REALTIME, 999, 1000000, &relative);
  Check(relative_ok && relative.tv_sec == 1 && relative.tv_nsec == 0,
        "relative deadlines retain their duration without an epoch");
  Check(MatchesNativeCpu(false), "ART current-thread CPU time matches GetThreadTimes");
  Check(MatchesNativeCpu(true), "ART process CPU time matches GetProcessTimes");
  uint64_t process_before = art::ProcessCpuNanoTime();
  uint64_t main_before = art::ThreadCpuNanoTime();
  std::atomic<bool> running{false}, done{false};
  bool worker_matches = false;
  uint64_t worker_delta = 0, checksum = 1;
  std::thread worker([&] {
    uint64_t before = NativeCpuTime(false);
    uint64_t limit = GetTickCount64() + 5000;
    bool matches = before != UINT64_MAX && MatchesNativeCpu(false);
    running.store(true, std::memory_order_release);
    while (GetTickCount64() < limit) {
      for (unsigned i = 0; i < 10000; ++i) checksum = checksum * UINT64_C(6364136223846793005) + 1;
      uint64_t now = NativeCpuTime(false);
      if (now != UINT64_MAX && now - before >= UINT64_C(50000000)) break;
    }
    uint64_t after = NativeCpuTime(false);
    worker_delta = before != UINT64_MAX && after != UINT64_MAX && after >= before ? after - before : 0;
    worker_matches = matches && MatchesNativeCpu(false);
    done.store(true, std::memory_order_release);
  });
  bool main_matches = true, process_matches = true;
  while (!running.load(std::memory_order_acquire)) Sleep(1);
  do {
    main_matches &= MatchesNativeCpu(false);
    process_matches &= MatchesNativeCpu(true);
    Sleep(1);
  } while (!done.load(std::memory_order_acquire));
  worker.join();
  uint64_t main_after = art::ThreadCpuNanoTime();
  uint64_t process_after = art::ProcessCpuNanoTime();
  Check(worker_matches && worker_delta >= UINT64_C(50000000),
        "a busy worker has its own measured CPU time");
  Check(main_matches && main_before != UINT64_MAX && main_after != UINT64_MAX && main_after >= main_before,
        "the waiting caller keeps its own CPU clock while another thread runs");
  Check(process_matches && process_before != UINT64_MAX && process_after != UINT64_MAX &&
            process_after >= process_before && process_after - process_before >= worker_delta,
        "the process CPU clock includes worker CPU time");
  std::printf("INFO  native3 clocks: worker %" PRIu64 ", caller %" PRIu64
              ", process %" PRIu64 " ns; checksum %" PRIu64 "\n",
              worker_delta, main_after - main_before, process_after - process_before, checksum);
  std::printf("%d test(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
