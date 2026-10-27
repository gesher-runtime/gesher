#include "query_pool.h"
#include <algorithm>
#include <limits>

namespace gesher::accessibility {
namespace {
thread_local bool g_executing_query = false;
}

int Query::RemainingMillis() const {
  if (!Active()) return 0;
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(left, 3000)));
}

bool QueryPool::Start() {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  std::unique_lock<std::timed_mutex> lifecycle(lifecycle_, std::defer_lock);
  if (!lifecycle.try_lock_until(deadline)) return false;
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_until(deadline) || stopping_) return false;
  if (started_) return accepting_;
  started_ = true;
  for (size_t i = 0; i < kWorkers; ++i) {
    threads_[i] = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
    if (threads_[i] == nullptr) {
      stopping_ = true;
      lock.unlock();
      changed_.notify_all();
      JoinThreads(deadline);  // life-cycle ownership stays serialized
      // Partial startup is not reported as a working provider.
      return false;
    }
  }
  accepting_ = true;
  return true;
}

void QueryPool::PruneLocked() {
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const auto& item) {
    return !item->Active();
  }), queue_.end());
}

bool QueryPool::Run(const std::shared_ptr<Query>& request) {
  // Nested callbacks must not consume every worker while waiting for that
  // same fixed pool. Rejection preserves the caller's original deadline.
  if (g_executing_query || request->submitted.exchange(true) || !request->Active()) return false;
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_until(request->deadline)) return false;
  PruneLocked();
  if (!request->Active() || !accepting_ || stopping_ || queue_.size() >= kQueued || next_token_ == 0) return false;
  request->token = next_token_++;
  queue_.push_back(request);
  lock.unlock();
  changed_.notify_one();
  std::unique_lock<std::mutex> result_lock(request->result_mutex);
  if (!request->result_ready.wait_until(result_lock, request->deadline, [&] { return request->done; })) {
    request->canceled.store(true, std::memory_order_release);
    if (mutex_.try_lock()) { PruneLocked(); mutex_.unlock(); }
    changed_.notify_all();
    return false;
  }
  return request->Active() && request->okay;
}

DWORD WINAPI QueryPool::ThreadEntry(void* value) {
  auto* self = static_cast<QueryPool*>(value);
  self->Worker(self->next_worker_.fetch_add(1));
  return 0;
}

void QueryPool::Worker(size_t slot) {
  if (slot >= kWorkers) return;
  for (;;) {
    std::shared_ptr<Query> request;
    {
      std::unique_lock<std::timed_mutex> lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      PruneLocked();
      if (stopping_ && queue_.empty()) return;
      if (queue_.empty()) continue;
      request = queue_.front();
      queue_.pop_front();
      active_[slot] = request;
    }
    // No pool/request/window-registry mutex is held across JNI, callbacks or
    // DetachCurrentThread. execute_ must destroy its ScopedEnv before return.
    if (request->Active()) {
      g_executing_query = true;
      execute_(*request);
      g_executing_query = false;
    }
    {
      std::lock_guard<std::mutex> result_lock(request->result_mutex);
      request->done = true;
    }
    request->result_ready.notify_all();
    {
      std::lock_guard<std::timed_mutex> lock(mutex_);
      active_[slot].reset();
    }
    changed_.notify_all();
  }
}

void QueryPool::Retire(const std::shared_ptr<WindowEpoch>& window) {
  if (!window) return;
  window->alive.store(false, std::memory_order_release);
  std::lock_guard<std::timed_mutex> lock(mutex_);
  for (const auto& request : active_) if (request && request->window == window) {
    request->canceled.store(true, std::memory_order_release);
  }
  PruneLocked();
  changed_.notify_all();
}

int QueryPool::Remaining(uint64_t token) {
  if (token == 0) return 0;
  std::lock_guard<std::timed_mutex> lock(mutex_);
  for (const auto& request : active_) if (request && request->token == token) return request->RemainingMillis();
  return 0;
}

bool QueryPool::Stop() {
  if (g_executing_query) return false;  // never wait for the current callback
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  std::unique_lock<std::timed_mutex> lifecycle(lifecycle_, std::defer_lock);
  if (!lifecycle.try_lock_until(deadline)) return false;
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_until(deadline)) return false;
  accepting_ = false;
  stopping_ = true;
  for (const auto& request : queue_) request->canceled.store(true, std::memory_order_release);
  queue_.clear();
  for (const auto& request : active_) if (request) request->canceled.store(true, std::memory_order_release);
  changed_.notify_all();
  if (!changed_.wait_until(lock, deadline, [&] {
        return std::none_of(active_.begin(), active_.end(), [](const auto& item) { return item != nullptr; });
      })) return false;
  lock.unlock();
  return JoinThreads(deadline);
}

bool QueryPool::JoinThreads(Clock::time_point deadline) {
  // Caller owns lifecycle_, so concurrent Stop never races/double-closes a
  // handle, even after the queue mutex is released for the bounded joins.
  for (HANDLE& thread : threads_) if (thread != nullptr) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left < 0 || WaitForSingleObject(thread, static_cast<DWORD>(left)) != WAIT_OBJECT_0) return false;
    CloseHandle(thread);
    thread = nullptr;
  }
  return true;
}

size_t QueryPool::ActiveCount() {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  return static_cast<size_t>(std::count_if(active_.begin(), active_.end(), [](const auto& item) { return item != nullptr; }));
}
size_t QueryPool::QueuedCount() {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  return queue_.size();
}
}  // namespace gesher::accessibility
