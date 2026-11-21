// gesher: timing summaries for the paths frames and sound take between an
// app's producer and the screen or the speakers (docs/media.md, "Measuring
// playback"). With GESHER_PERF_TRACE set in the device's environment, each
// site prints, every few seconds it is used, one line to stderr (the process's
// log): how many times it ran, how long it took on average and at most, and a
// sum of an optional count (frames dropped, frames missing).
//
//   static gesher::perf::Site site("surface forward: copy + transaction");
//   int64_t start = gesher::perf::Now();
//   ...
//   site.Add(gesher::perf::Now() - start);
//
// Off (the default), Add and Now cost a load and a branch. Header-only, so
// every library (the framework's natives, media, the bionic shim) can use it.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <time.h>

#include <chrono>
#include <ctime>
#include <mutex>

namespace gesher::perf {

inline bool Enabled() {
  static const bool on = getenv("GESHER_PERF_TRACE") != nullptr;
  return on;
}

// Monotonic nanoseconds; 0 when tracing is off.
inline int64_t Now() {
  if (!Enabled()) return 0;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class Site {
 public:
  explicit constexpr Site(const char* name, int64_t period_ns = 5'000'000'000)
      : name_(name), period_ns_(period_ns) {}

  // One run that took `ns` (from Now() differences), with `count` added to
  // the period's sum (frames dropped, underruns, ...).
  void Add(int64_t ns, int64_t count = 0) {
    if (!Enabled()) return;
    const int64_t now = Now();
    if (ns >= kSlowNs) {
      // A run this long is a visible stall: said at once, with the wall-clock
      // time (to set beside the logcat lines of other processes).
      const auto wall = std::chrono::system_clock::now();
      const time_t t = std::chrono::system_clock::to_time_t(wall);
      const long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            wall.time_since_epoch()).count() % 1000);
      struct tm local {};
      localtime_s(&local, &t);
      fprintf(stderr, "gesher perf: %02d:%02d:%02d.%03ld %s: one run took %.1f ms\n", local.tm_hour,
              local.tm_min, local.tm_sec, ms, name_, static_cast<double>(ns) / 1e6);
      fflush(stderr);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (start_ == 0) start_ = now;
    ++runs_;
    total_ns_ += ns;
    if (ns > max_ns_) max_ns_ = ns;
    sum_ += count;
    if (now - start_ < period_ns_) return;
    const double seconds = static_cast<double>(now - start_) / 1e9;
    fprintf(stderr,
            "gesher perf: %s: %lld in %.1f s (%.1f/s), avg %.3f ms, max %.3f ms, count %lld\n",
            name_, static_cast<long long>(runs_), seconds, static_cast<double>(runs_) / seconds,
            static_cast<double>(total_ns_) / static_cast<double>(runs_) / 1e6,
            static_cast<double>(max_ns_) / 1e6, static_cast<long long>(sum_));
    fflush(stderr);
    start_ = now;
    runs_ = 0;
    total_ns_ = 0;
    max_ns_ = 0;
    sum_ = 0;
  }

 private:
  static constexpr int64_t kSlowNs = 100'000'000;  // 100 ms
  const char* name_;
  const int64_t period_ns_;
  std::mutex mutex_;
  int64_t start_ = 0;
  int64_t runs_ = 0;
  int64_t total_ns_ = 0;
  int64_t max_ns_ = 0;
  int64_t sum_ = 0;
};

}  // namespace gesher::perf
