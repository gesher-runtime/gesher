// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "base/time_utils.h"

#include <windows.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <thread>

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
}  // namespace

int main() {
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
