// Coalesce posted UI invalidations; a failed post must leave the next frame retryable.
#pragma once

#include <atomic>
#include <utility>

namespace gesher::media {

class FrameInvalidation {
 public:
  enum class Result { kPosted, kCoalesced, kNoEnv, kNoView, kJavaFailure };

  template <typename Post>
  Result Request(Post&& post) {
    if (pending_.exchange(true, std::memory_order_acq_rel)) return Result::kCoalesced;
    const Result result = std::forward<Post>(post)();
    if (result != Result::kPosted) pending_.store(false, std::memory_order_release);
    return result;
  }

  template <typename Bind>
  void Acknowledge(Bind&& bind) {
    std::forward<Bind>(bind)();
    pending_.store(false, std::memory_order_release);
  }

 private:
  std::atomic<bool> pending_{false};
};

}  // namespace gesher::media
