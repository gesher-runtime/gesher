// Serialized native codec callbacks. The consumer owns this queue until its
// worker exits; Close also waits for an in-flight callback on another thread.
// A callback may close its own codec without joining or freeing its own stack.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace gesher::media {

class CodecCallbackQueue {
 public:
  static std::shared_ptr<CodecCallbackQueue> Create(
      std::function<void()> initialize) {
    auto queue = std::shared_ptr<CodecCallbackQueue>(new CodecCallbackQueue);
    std::thread([queue, initialize = std::move(initialize)] {
      if (initialize) initialize();
      queue->Run();
    }).detach();
    return queue;
  }

  void Post(std::function<void()> callback) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock,
             [this] { return closed_ || paused_ || pending_.size() < 64; });
    if (closed_ || paused_) return;
    pending_.push_back(std::move(callback));
    cv_.notify_all();
  }

  void Pause() {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = true;
    pending_.clear();
    cv_.notify_all();
  }

  void Resume() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!closed_) paused_ = false;
  }

  void Close() {
    std::unique_lock<std::mutex> lock(mu_);
    closed_ = true;
    pending_.clear();
    cv_.notify_all();
    if (worker_ == std::this_thread::get_id()) return;
    cv_.wait(lock, [this] { return exited_; });
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> lock(mu_);
    worker_ = std::this_thread::get_id();
    for (;;) {
      cv_.wait(lock, [this] { return closed_ || !pending_.empty(); });
      if (closed_) break;
      auto callback = std::move(pending_.front());
      pending_.pop_front();
      cv_.notify_all();
      lock.unlock();
      callback();
      lock.lock();
    }
    exited_ = true;
    cv_.notify_all();
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> pending_;
  std::thread::id worker_;
  bool paused_ = false, closed_ = false, exited_ = false;
};

}  // namespace gesher::media
