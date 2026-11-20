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
#include <thread>
#include <vector>

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
  return failures.load() != 0;
}
