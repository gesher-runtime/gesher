// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/bionic/binary128_parse.h"

#include <atomic>
#include <barrier>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

char* Parse(const char* text, uint64_t output[2]) {
  char* end = nullptr;
  gesher::bionic::ParseBinary128(text, &end, 1 /* nearest */, output);
  return end;
}

double Seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Bionic holds gdtoa's locks only around shared-cache updates, so a very long
// conversion does not stall other threads' conversions for its duration.
unsigned LongParseContention(const char* midpoint, size_t midpoint_length) {
  std::string huge = "0.";
  for (unsigned i = 0; i < 200000; ++i) huge += "1428570369"[i % 10];
  std::atomic<unsigned> failures{0};
  // The serial run is the reference result; it also grows the shared
  // powers-of-five cache, as the short conversion's first run does for it.
  uint64_t reference[2], expected_short[2];
  auto serial = std::chrono::steady_clock::now();
  if (Parse(huge.c_str(), reference) != huge.c_str() + huge.size()) ++failures;
  const double long_seconds = Seconds(serial);
  if (Parse(midpoint, expected_short) != midpoint + midpoint_length) ++failures;
  if (long_seconds < 0.05) {
    std::printf("FAIL long conversion took only %.3fs; contention is not observable\n", long_seconds);
    return failures.load() + 1;
  }
  std::atomic<int> state{0};
  uint64_t concurrent[2] = {};
  std::thread long_parse([&] {
    state.store(1);
    if (Parse(huge.c_str(), concurrent) != huge.c_str() + huge.size()) ++failures;
    state.store(2);
  });
  while (state.load() == 0) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::duration<double>(long_seconds / 10));
  unsigned during = 0;
  double worst = 0;
  while (state.load() != 2) {
    auto start = std::chrono::steady_clock::now();
    uint64_t output[2];
    if (Parse(midpoint, output) != midpoint + midpoint_length ||
        output[0] != expected_short[0] || output[1] != expected_short[1]) ++failures;
    double elapsed = Seconds(start);
    if (state.load() != 2) {
      ++during;
      if (elapsed > worst) worst = elapsed;
    }
  }
  long_parse.join();
  if (concurrent[0] != reference[0] || concurrent[1] != reference[1]) ++failures;
  std::printf("Long conversion %.3fs serial; %u short conversions completed during it "
              "(slowest %.3f ms)\n", long_seconds, during, worst * 1000);
  if (during == 0) {
    std::printf("FAIL a short conversion waited for the whole long conversion\n");
    ++failures;
  }
  return failures.load();
}

}  // namespace

int main() {
  // Exactly 1 + 2^-113: a binary128 midpoint, with enough decimal digits to
  // exercise gdtoa's shared bigint caches in every thread.
  constexpr char midpoint[] =
      "1.000000000000000000000000000000000096296497219361792652798897129246"
      "36592690508241076940976199693977832794189453125";
  constexpr unsigned workers = 8, iterations = 10000;
  const int modes[] = {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
  const int caller_mode = std::fegetround();
  std::barrier start(workers);
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> threads;
  auto before = std::chrono::steady_clock::now();
  for (unsigned thread = 0; thread < workers; ++thread) {
    threads.emplace_back([&, thread] {
      int mode = thread % 4;
      if (std::fesetround(modes[mode]) != 0) ++failures;
      start.arrive_and_wait();
      for (unsigned i = 0; i < iterations; ++i) {
        errno = 123;
        char* end = nullptr;
        uint64_t output[2];
        gesher::bionic::ParseBinary128(midpoint, &end,
                                      gesher::bionic::Binary128RoundingMode(), output);
        if (output[0] != (mode == 2 ? 1u : 0u) || output[1] != UINT64_C(0x3fff000000000000) ||
            end != midpoint + sizeof(midpoint) - 1 || errno != 123 ||
            std::fegetround() != modes[mode]) ++failures;
      }
    });
  }
  for (auto& thread : threads) thread.join();
  if (std::fegetround() != caller_mode) ++failures;
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
  std::printf("Binary128 concurrent parsing: %u threads, %u calls, %.3fs, %u failures\n",
              workers, workers * iterations, seconds, failures.load());
  unsigned contention = LongParseContention(midpoint, sizeof(midpoint) - 1);
  return failures.load() != 0 || contention != 0;
}
