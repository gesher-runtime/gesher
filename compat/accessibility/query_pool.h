// Bounded native handoff before any UIA caller enters ART/JNI.
#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace gesher::accessibility {

using Clock = std::chrono::steady_clock;
struct WindowEpoch {
  explicit WindowEpoch(uint64_t value) : generation(value) {}
  const uint64_t generation;
  std::atomic<bool> alive{true};
};
enum class QueryKind { kNode, kNodeAt, kFocused, kPerform };
struct Query {
  // Only native value data/shared lifetime state crosses the handoff: never
  // a JNIEnv, local Java reference, Window or Surface pointer.
  QueryKind kind = QueryKind::kNode;
  uintptr_t hwnd = 0;
  int64_t id = 0;
  int32_t x = 0, y = 0, action = 0;
  std::wstring text;
  std::shared_ptr<WindowEpoch> window;
  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(3);
  uint64_t token = 0;
  std::atomic<bool> canceled{false};
  std::atomic<bool> submitted{false};
  std::mutex result_mutex;
  std::condition_variable result_ready;
  bool done = false;
  std::wstring node;
  int64_t answer = 0;
  bool okay = false;

  bool Active() const {
    return !canceled.load(std::memory_order_acquire) && window &&
           window->alive.load(std::memory_order_acquire) && Clock::now() < deadline;
  }
  int RemainingMillis() const;
};

class QueryPool {
 public:
  using Execute = void (*)(Query&) noexcept;
  static constexpr size_t kWorkers = 4, kQueued = 32;
  // Optional allocator boundary lets the CI fixture fail the second actual
  // thread allocation after a real first worker exists. Production uses Win32.
  using Spawn = HANDLE (*)(LPTHREAD_START_ROUTINE, void*);
  explicit QueryPool(Execute execute, Spawn spawn = nullptr) : execute_(execute), spawn_(spawn) {}
  // This object is process-owned; it is never destroyed underneath a worker.
  // Stop() closes worker handles only after every callback has returned and
  // detached from ART. No static destructor performs a join.
  bool Start();
  bool Run(const std::shared_ptr<Query>& request);
  void Retire(const std::shared_ptr<WindowEpoch>& window);
  int Remaining(uint64_t token);
  bool Stop();
  size_t ActiveCount();
  size_t QueuedCount();

 private:
  static DWORD WINAPI ThreadEntry(void* self);
  void Worker(size_t slot);
  void PruneLocked();
  bool JoinThreads(Clock::time_point deadline);
  Execute execute_;
  Spawn spawn_;
  std::timed_mutex mutex_;
  std::timed_mutex lifecycle_;
  std::condition_variable_any changed_;
  std::array<HANDLE, kWorkers> threads_{};
  std::array<std::shared_ptr<Query>, kWorkers> active_{};
  std::deque<std::shared_ptr<Query>> queue_;
  std::atomic<size_t> next_worker_{0};
  uint64_t next_token_ = 1;
  bool started_ = false, accepting_ = false, stopping_ = false;
};

}  // namespace gesher::accessibility
