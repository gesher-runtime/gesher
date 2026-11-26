// Opt-in production futex microbenchmark, shared by futex_test.exe and the
// host A/B runner. It is not a timing gate or an application-speed estimate.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

using FutexBenchApi = long (*)(volatile int*, int, int, const timespec*, volatile int*, int);
static double futex_bench_cpu_seconds() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
  ULARGE_INTEGER k, u;
  k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
  return static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
#else
  timespec value;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0) return -1;
  return static_cast<double>(value.tv_sec) + value.tv_nsec / 1000000000.0;
#endif
}

static int futex_contention_benchmark(FutexBenchApi api, int iterations = 100000,
                                     int samples = 5) {
  if (iterations <= 0 || samples <= 0) return 2;
  // Within one aligned page these 64 words have different low-six-bit keys
  // and unchanged upper hash bits, so each uses a different production bucket.
  // No benchmark worker changes a word or includes allocation in its loop.
  alignas(4096) int words[64] = {};
  struct Stats { unsigned long long locks = 0, contended = 0, errors = 0; };
  int failed = 0;
  for (int sample = 0; sample < samples; ++sample) {
    for (int count : {1, 2, 8, 32}) for (bool shared : {false, true}) {
      for (bool mismatch : {false, true}) {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<Stats> results(count);
        std::vector<std::thread> threads;
        for (int i = 0; i < count; ++i) threads.emplace_back([&, i] {
          volatile int* word = &words[shared ? 0 : i];
          ready.fetch_add(1, std::memory_order_release);
          while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
          Stats local;
          for (int op = 0; op < iterations; ++op) {
            long result = api(word,mismatch ? FUTEX_WAIT_PRIVATE : FUTEX_WAKE_PRIVATE,
                              1,nullptr,nullptr,0);
            if (mismatch ? (result != -1 || errno != EAGAIN) : result != 0) ++local.errors;
          }
#ifdef FUTEX_BENCH_LOCK_COUNTS
          local.locks = futex_bench_acquisitions;
          local.contended = futex_bench_contentions;
#endif
          results[i] = local;
        });
        while (ready.load(std::memory_order_acquire) != count) std::this_thread::yield();
        double cpu_start = futex_bench_cpu_seconds();
        auto wall_start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
        double cpu_end = futex_bench_cpu_seconds();
        Stats total;
        for (const Stats& result : results) {
          total.locks += result.locks;
          total.contended += result.contended;
          total.errors += result.errors;
        }
        failed |= total.errors != 0;
        std::printf("{\"sample\":%d,\"threads\":%d,\"shared\":%s,\"operation\":\"%s\","
                    "\"operations\":%llu,\"wall_seconds\":%.9f,\"cpu_seconds\":",
                    sample,count,shared ? "true" : "false",mismatch ? "mismatch" : "wake",
                    static_cast<unsigned long long>(count) * iterations,wall);
        if (cpu_start < 0 || cpu_end < cpu_start) std::printf("null");
        else std::printf("%.9f",cpu_end - cpu_start);
#ifdef FUTEX_BENCH_LOCK_COUNTS
        std::printf(",\"lock_attempts\":%llu,\"contended_acquisitions\":%llu",total.locks,total.contended);
#else
        std::printf(",\"lock_attempts\":null,\"contended_acquisitions\":null");
#endif
        std::printf(",\"errors\":%llu}\n",total.errors);
      }
    }
  }
  return failed;
}
