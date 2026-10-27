// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "../audio/log.h"

namespace gesher::media {

// Opt-in observation of asynchronous preparation. Declare before any local
// guards so that the slow-path log is written after those guards are released.
class PrepareTrace {
 public:
  explicit PrepareTrace(const char* operation)
      : operation_(operation), active_(Enabled()) {
    if (active_) start_ = last_ = Clock::now();
  }
  void Step(const char* label) {
    if (!active_) return;
    const auto now = Clock::now();
    if (count_ < 8) {
      labels_[count_] = label;
      times_[count_++] = Micros(now - last_);
    } else { truncated_ = true; }
    last_ = now;
  }
  void Complete() { complete_ = true; }
  ~PrepareTrace() {
    if (!active_) return;
    const auto end = Clock::now();
    const long long wall = Micros(end - start_);
    if (wall < 100000 || Lines().fetch_add(1u, std::memory_order_relaxed) >= 16) return;
    char phases[768] = {};
    size_t used = 0;
    for (size_t i = 0; i < count_; ++i) {
      const int wrote = std::snprintf(phases + used, sizeof(phases) - used,
                                      "%s%s %lld", i == 0 ? "" : ", ", labels_[i], times_[i]);
      if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof(phases) - used) break;
      used += static_cast<size_t>(wrote);
    }
    MEDIA_LOGI("player prepare trace: %s, %s, steady wall us %lld, phases%s [%s], tail %lld",
               operation_, complete_ ? "complete" : "incomplete/error", wall,
               truncated_ ? " [truncated]" : "", phases,
               Micros(end - last_));
  }
 private:
  using Clock = std::chrono::steady_clock;
  static long long Micros(Clock::duration elapsed) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  }
  static bool Enabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("GESHER_MEDIA_TRACE_PREPARE");
      return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }
  static std::atomic<unsigned>& Lines() {
    static std::atomic<unsigned> lines{0};
    return lines;
  }
  const char* operation_;
  const bool active_;
  bool complete_ = false;
  bool truncated_ = false;
  size_t count_ = 0;
  Clock::time_point start_{}, last_{};
  const char* labels_[8] = {};
  long long times_[8] = {};
};

}  // namespace gesher::media
