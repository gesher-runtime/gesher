// Coalesce posted UI invalidations; a failed post must leave the next frame retryable.
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace gesher::media {

class FrameInvalidation {
 public:
  enum class Result { kPosted, kCoalesced, kNoEnv, kNoView, kJavaFailure };

  template <typename Post>
  Result Request(Post&& post) {
    uint64_t observed = state_.load(std::memory_order_acquire);
    for (;;) {
      if (observed & 1) return Result::kCoalesced;
      if (state_.compare_exchange_weak(observed, observed | 1, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) break;
    }
    const uint64_t ticket = observed | 1;
    const Result result = std::forward<Post>(post)();
    if (result != Result::kPosted) {
      // A draw and a newer successful post may have overtaken this callback.
      // Only release the pending request owned by this exact generation.
      uint64_t expected = ticket;
      state_.compare_exchange_strong(expected, ticket + 1, std::memory_order_release,
                                      std::memory_order_relaxed);
    }
    return result;
  }

  template <typename Bind>
  void Acknowledge(Bind&& bind) {
    std::forward<Bind>(bind)();
    uint64_t observed = state_.load(std::memory_order_relaxed);
    while (!state_.compare_exchange_weak(observed, (observed | 1) + 1,
                                         std::memory_order_release, std::memory_order_relaxed)) {}
  }

 private:
  // Low bit: pending. Other bits: generation, advanced by every draw/release.
  std::atomic<uint64_t> state_{0};
};

}  // namespace gesher::media
