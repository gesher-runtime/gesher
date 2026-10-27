// CI only: the production scheduling gate under controlled post failures and binding order.
#include "compat/media/surface/frame_invalidation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using gesher::media::FrameInvalidation;
using Result = FrameInvalidation::Result;
int failed = 0;

void Check(bool ok, const char* name) {
  std::printf("surface invalidation %s: %s\n", name, ok ? "ok" : "FAILED");
  if (!ok) ++failed;
}

void FailedPost(Result failure, const char* name) {
  FrameInvalidation gate;
  int attempts = 0;
  const Result missing = gate.Request([&] { ++attempts; return failure; });
  const Result recovered = gate.Request([&] { ++attempts; return Result::kPosted; });
  const Result pending = gate.Request([&] { ++attempts; return Result::kPosted; });
  Check(missing == failure && recovered == Result::kPosted &&
            pending == Result::kCoalesced && attempts == 2, name);
}

void CoalesceUntilDraw() {
  FrameInvalidation gate;
  int posts = 0, bindings = 0;
  auto post = [&] { ++posts; return Result::kPosted; };
  const Result first = gate.Request(post);
  const Result second = gate.Request(post);
  gate.Acknowledge([&] { ++bindings; });
  const Result next = gate.Request(post);
  Check(first == Result::kPosted && second == Result::kCoalesced &&
            next == Result::kPosted && posts == 2 && bindings == 1, "coalesce-until-draw");
}

void BindBeforeAcknowledge() {
  FrameInvalidation gate;
  gate.Request([] { return Result::kPosted; });
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, release = false;
  std::atomic<bool> bound{false};
  std::thread draw([&] {
    gate.Acknowledge([&] {
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      changed.notify_one();
      changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
      bound.store(true, std::memory_order_release);
    });
  });
  bool waiting;
  {
    std::unique_lock<std::mutex> lock(mutex);
    waiting = changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
  }
  int early = 0;
  const Result during = gate.Request([&] {
    ++early;
    return bound.load(std::memory_order_acquire) ? Result::kPosted : Result::kNoView;
  });
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  changed.notify_one();
  draw.join();
  const Result after = gate.Request([&] {
    return bound.load(std::memory_order_acquire) ? Result::kPosted : Result::kNoView;
  });
  Check(waiting && during == Result::kCoalesced && early == 0 &&
            after == Result::kPosted, "bind-before-acknowledge");
}

void ConcurrentProducers() {
  FrameInvalidation gate;
  std::atomic<int> posts{0};
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
      workers.emplace_back([&] {
        gate.Request([&] { posts.fetch_add(1, std::memory_order_relaxed); return Result::kPosted; });
      });
    }
    for (auto& worker : workers) worker.join();
    gate.Acknowledge([] {});
  }
  Check(posts.load() == 2, "concurrent-producers-coalesce");
}

}  // namespace

int main() {
  FailedPost(Result::kNoEnv, "no-env-then-recovery");
  FailedPost(Result::kNoView, "no-view-then-recovery");
  FailedPost(Result::kJavaFailure, "java-post-failure-then-recovery");
  CoalesceUntilDraw();
  BindBeforeAcknowledge();
  ConcurrentProducers();
  std::printf("surface invalidation: %d failure(s)\n", failed);
  return failed == 0 ? 0 : 1;
}
