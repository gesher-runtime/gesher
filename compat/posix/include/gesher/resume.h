// Suspend-aware waits opt into the runtime's Windows resume notification.
#pragma once

#include <memory>

namespace gesher {

struct ResumeObserver;

class ResumeSubscription {
 public:
  ResumeSubscription() = default;
  ~ResumeSubscription();
  ResumeSubscription(const ResumeSubscription&) = delete;
  ResumeSubscription& operator=(const ResumeSubscription&) = delete;

  // Zero on success, otherwise an errno value. A failed subscription must not
  // be used to promise BOOTTIME wakeups. Start/Reset are serialized by caller.
  int Start(void (*callback)(void*), void* context);

  // Disables and joins this consumer's callbacks. Call with no consumer locks
  // held, and never from its own callback. Safe to call more than once.
  void Reset();

 private:
  std::shared_ptr<ResumeObserver> observer_;
};

}  // namespace gesher
