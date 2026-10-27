// Opt-in camera-to-recorder timing diagnostics (GESHER_MEDIA_TRACE_RECORD=1).
// Times native work separately from Java/JNI transitions; no capture policy changes.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>

namespace gesher::devices::camera {

class TraceSample;

class TraceStats {
 public:
  TraceStats(const char* label, std::initializer_list<const char*> phases) : label_(label) {
    size_t i = 0;
    for (const char* phase : phases) names_[i++] = phase;
    count_ = i;
  }

  void Log() {
    if (!Enabled()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (calls_ == 0) return;
    std::fprintf(stderr, "camera native trace: %s, calls %llu, wall total/max us %lld/%lld; [",
                 label_, static_cast<unsigned long long>(calls_),
                 static_cast<long long>(wall_total_ / 1000),
                 static_cast<long long>(wall_max_ / 1000));
    for (size_t i = 0; i < count_; ++i) {
      std::fprintf(stderr, "%s%s %lld/%lld", i == 0 ? "" : ", ", names_[i],
                   static_cast<long long>(total_[i] / 1000),
                   static_cast<long long>(maximum_[i] / 1000));
    }
    std::fprintf(stderr, "]\n");
    std::fflush(stderr);
  }

 private:
  friend class TraceSample;
  static constexpr size_t kPhases = 8;
  using Times = std::array<int64_t, kPhases>;

  static bool Enabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("GESHER_MEDIA_TRACE_RECORD");
      return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  void Add(const Times& times, int64_t wall) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++calls_;
    wall_total_ += wall;
    wall_max_ = std::max(wall_max_, wall);
    for (size_t i = 0; i < count_; ++i) {
      total_[i] += times[i];
      maximum_[i] = std::max(maximum_[i], times[i]);
    }
  }

  const char* label_;
  std::array<const char*, kPhases> names_{};
  size_t count_ = 0;
  std::mutex mutex_;
  uint64_t calls_ = 0;
  int64_t wall_total_ = 0, wall_max_ = 0;
  Times total_{}, maximum_{};
};

class TraceSample {
 public:
  explicit TraceSample(TraceStats& stats) : stats_(stats), enabled_(TraceStats::Enabled()) {
    if (enabled_) start_ = last_ = Clock::now();
  }
  ~TraceSample() {
    if (enabled_) stats_.Add(times_, Nanos(Clock::now() - start_));
  }

  void Step(size_t phase) {
    if (!enabled_) return;
    auto now = Clock::now();
    times_[phase] += Nanos(now - last_);
    last_ = now;
  }

 private:
  using Clock = std::chrono::steady_clock;
  static int64_t Nanos(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
  }
  TraceStats& stats_;
  bool enabled_;
  Clock::time_point start_{}, last_{};
  TraceStats::Times times_{};
};

}  // namespace gesher::devices::camera
