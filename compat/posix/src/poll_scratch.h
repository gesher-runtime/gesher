// Reusable working storage for one poll invocation. No readiness or descriptor
// ownership survives the invocation, and recursion never borrows active storage.
#pragma once

#include <cstddef>
#include <vector>

namespace gesher {

inline constexpr size_t kPollScratchLimit = 256;

template <typename T>
void ClearPollScratch(std::vector<T>& values) {
  values.clear();
  if (values.capacity() > kPollScratchLimit) std::vector<T>().swap(values);
}

// Scratch::Reset clears every element (including owning references) and drops
// oversized capacity. Keep busy set until that cleanup has finished: releasing
// an owned object may itself reenter poll. Only one small bundle per type/thread
// is retained; large and nested invocations have ordinary local lifetimes.
template <typename Scratch>
class PollScratchLease {
 public:
  explicit PollScratchLease(bool cache_allowed = true) {
    if (!cache_allowed) return;
    static thread_local Cache cache;
    if (!cache.busy) {
      cache.busy = true;
      cached_ = &cache;
      scratch_ = &cache.scratch;
    }
  }
  ~PollScratchLease() {
    scratch_->Reset();
    if (cached_ != nullptr) cached_->busy = false;
  }
  PollScratchLease(const PollScratchLease&) = delete;
  PollScratchLease& operator=(const PollScratchLease&) = delete;
  Scratch& get() { return *scratch_; }

 private:
  struct Cache {
    Scratch scratch;
    bool busy = false;
  };
  Scratch local_;
  Scratch* scratch_ = &local_;
  Cache* cached_ = nullptr;
};

}  // namespace gesher
