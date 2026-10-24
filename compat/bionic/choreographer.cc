// gesher: libandroid's AChoreographer for an app's ELF libraries
// (docs/native-libs.md, "NativeActivity"; ADR 0032).
//
// On Android a thread's AChoreographer (frameworks/native/libs/nativedisplay)
// gets vsync events from SurfaceFlinger through a DisplayEventReceiver on the
// thread's Looper. Gesher has no compositor: the display's vsync is a 60 Hz
// clock (CLOCK_MONOTONIC, frames at multiples of 16,666,667 ns), and each
// thread's choreographer is a MessageHandler on that thread's Looper
// (ALooper_prepare's, or Java's on the main thread) that wakes at the next
// vsync with callbacks due and runs them there, as AOSP's does when the
// Looper is polled. Callbacks are System V functions of loaded code.
//
//  - postFrameCallback(64)/postFrameCallbackDelayed(64): the callback runs at
//    the first vsync at or after the delay, with that vsync's time;
//  - postVsyncCallback: with an AChoreographerFrameCallbackData of one frame
//    timeline (vsync id, expected presentation one frame later, deadline);
//  - registerRefreshRateCallback: told the 60 Hz period once, on the Looper.

#include "jni_bridge.h"
#include "shim_internal.h"

#include <gesher/bionic_shim.h>

#include <utils/Looper.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace gesher::bionic {
namespace {

using android::Looper;
using android::sp;

constexpr nsecs_t kVsyncPeriod = 16'666'667;  // 60 Hz

nsecs_t NextVsyncAtOrAfter(nsecs_t t) { return (t + kVsyncPeriod - 1) / kVsyncPeriod * kVsyncPeriod; }

using SysvFrameCallback = void(BIONIC_SYSV*)(int64_t frameTimeNanos, void* data);
// AChoreographerFrameCallbackData* first.
using SysvVsyncCallback = void(BIONIC_SYSV*)(const void* callbackData, void* data);
using SysvRefreshRateCallback = void(BIONIC_SYSV*)(int64_t vsyncPeriodNanos, void* data);

// What AChoreographerFrameCallbackData_* read: one frame timeline.
struct FrameCallbackData {
  int64_t frame_time;
  int64_t vsync_id;
  int64_t expected_presentation;
  int64_t deadline;
};

class Choreographer : public android::MessageHandler {
 public:
  explicit Choreographer(const sp<Looper>& looper) : looper_(looper) {}

  void post(bool vsync, void* fn, void* data, nsecs_t delay) {
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    android::Mutex::Autolock _l(lock_);
    callbacks_.push_back({vsync, fn, data, NextVsyncAtOrAfter(now + std::max<nsecs_t>(delay, 0) + 1)});
    scheduleLocked();
  }

  void addRefreshRateCallback(void* fn, void* data) {
    android::Mutex::Autolock _l(lock_);
    for (const auto& r : refresh_) {
      if (r.first == fn && r.second == data) return;
    }
    refresh_.emplace_back(fn, data);
    pending_refresh_.emplace_back(fn, data);  // told the period once
    looper_->sendMessage(sp<android::MessageHandler>::fromExisting(this), android::Message(kRefresh));
  }

  void removeRefreshRateCallback(void* fn, void* data) {
    android::Mutex::Autolock _l(lock_);
    auto same = [&](const std::pair<void*, void*>& r) { return r.first == fn && r.second == data; };
    refresh_.erase(std::remove_if(refresh_.begin(), refresh_.end(), same), refresh_.end());
    pending_refresh_.erase(std::remove_if(pending_refresh_.begin(), pending_refresh_.end(), same),
                           pending_refresh_.end());
  }

  void handleMessage(const android::Message& message) override {
    EnterLoadedCode();
    if (message.what == kRefresh) {
      std::vector<std::pair<void*, void*>> told;
      {
        android::Mutex::Autolock _l(lock_);
        told.swap(pending_refresh_);
      }
      for (const auto& r : told) reinterpret_cast<SysvRefreshRateCallback>(r.first)(kVsyncPeriod, r.second);
      return;
    }
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    nsecs_t vsync = now / kVsyncPeriod * kVsyncPeriod;  // this frame's vsync
    std::vector<Callback> due;
    {
      android::Mutex::Autolock _l(lock_);
      scheduled_ = 0;
      auto split = std::stable_partition(callbacks_.begin(), callbacks_.end(),
                                         [&](const Callback& c) { return c.due > now; });
      due.assign(split, callbacks_.end());
      callbacks_.erase(split, callbacks_.end());
      ++vsync_id_;
    }
    FrameCallbackData frame{vsync, vsync_id_, vsync + kVsyncPeriod, vsync + kVsyncPeriod};
    for (const Callback& c : due) {
      if (c.vsync) {
        reinterpret_cast<SysvVsyncCallback>(c.fn)(&frame, c.data);
      } else {
        reinterpret_cast<SysvFrameCallback>(c.fn)(vsync, c.data);
      }
    }
    android::Mutex::Autolock _l(lock_);
    scheduleLocked();  // callbacks posted meanwhile, or delayed ones
  }

 private:
  static constexpr int kFrame = 1;
  static constexpr int kRefresh = 2;

  struct Callback {
    bool vsync;
    void* fn;
    void* data;
    nsecs_t due;
  };

  void scheduleLocked() {
    if (callbacks_.empty()) return;
    nsecs_t first = callbacks_.front().due;
    for (const Callback& c : callbacks_) first = std::min(first, c.due);
    if (scheduled_ != 0 && scheduled_ <= first) return;
    sp<android::MessageHandler> self = sp<android::MessageHandler>::fromExisting(this);
    if (scheduled_ != 0) looper_->removeMessages(self, kFrame);
    scheduled_ = first;
    looper_->sendMessageAtTime(first, self, android::Message(kFrame));
  }

  sp<Looper> looper_;
  android::Mutex lock_;
  std::vector<Callback> callbacks_;
  std::vector<std::pair<void*, void*>> refresh_;
  std::vector<std::pair<void*, void*>> pending_refresh_;
  nsecs_t scheduled_ = 0;
  int64_t vsync_id_ = 0;
};

// One per thread, as AOSP's (a thread_local), for as long as the thread lives.
thread_local Choreographer* t_choreographer = nullptr;

Choreographer* C(void* choreographer) { return static_cast<Choreographer*>(choreographer); }

void* BIONIC_SYSV GetInstance() {
  if (t_choreographer == nullptr) {
    sp<Looper> looper = Looper::getForThread();
    if (looper == nullptr) return nullptr;  // as on Android: the thread needs a Looper
    t_choreographer = new Choreographer(looper);
    t_choreographer->incStrong(&t_choreographer);
  }
  return t_choreographer;
}

void BIONIC_SYSV PostFrameCallback(void* c, void* cb, void* data) {
  C(c)->post(false, cb, data, 0);
}
void BIONIC_SYSV PostFrameCallbackDelayed(void* c, void* cb, void* data, int64_t delayMillis) {
  C(c)->post(false, cb, data, ms2ns(delayMillis));
}
void BIONIC_SYSV PostFrameCallbackDelayed64(void* c, void* cb, void* data, uint32_t delayMillis) {
  C(c)->post(false, cb, data, ms2ns(static_cast<nsecs_t>(delayMillis)));
}
void BIONIC_SYSV PostVsyncCallback(void* c, void* cb, void* data) {
  C(c)->post(true, cb, data, 0);
}
void BIONIC_SYSV RegisterRefreshRateCallback(void* c, void* cb, void* data) {
  C(c)->addRefreshRateCallback(cb, data);
}
void BIONIC_SYSV UnregisterRefreshRateCallback(void* c, void* cb, void* data) {
  C(c)->removeRefreshRateCallback(cb, data);
}

const FrameCallbackData* D(const void* d) { return static_cast<const FrameCallbackData*>(d); }
int64_t BIONIC_SYSV DataGetFrameTimeNanos(const void* d) { return D(d)->frame_time; }
size_t BIONIC_SYSV DataGetFrameTimelinesLength(const void*) { return 1; }
size_t BIONIC_SYSV DataGetPreferredFrameTimelineIndex(const void*) { return 0; }
int64_t BIONIC_SYSV DataGetFrameTimelineVsyncId(const void* d, size_t) { return D(d)->vsync_id; }
int64_t BIONIC_SYSV DataGetFrameTimelineExpectedPresentationTimeNanos(const void* d, size_t) {
  return D(d)->expected_presentation;
}
int64_t BIONIC_SYSV DataGetFrameTimelineDeadlineNanos(const void* d, size_t) {
  return D(d)->deadline;
}

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
  static const Entry kEntries[] = {
      E("AChoreographer_getInstance", GetInstance),
      // `long` frame times are 64-bit on LP64 bionic: the 64 forms are the same.
      E("AChoreographer_postFrameCallback", PostFrameCallback),
      E("AChoreographer_postFrameCallback64", PostFrameCallback),
      E("AChoreographer_postFrameCallbackDelayed", PostFrameCallbackDelayed),
      E("AChoreographer_postFrameCallbackDelayed64", PostFrameCallbackDelayed64),
      E("AChoreographer_postVsyncCallback", PostVsyncCallback),
      E("AChoreographer_registerRefreshRateCallback", RegisterRefreshRateCallback),
      E("AChoreographer_unregisterRefreshRateCallback", UnregisterRefreshRateCallback),
      E("AChoreographerFrameCallbackData_getFrameTimeNanos", DataGetFrameTimeNanos),
      E("AChoreographerFrameCallbackData_getFrameTimelinesLength", DataGetFrameTimelinesLength),
      E("AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex",
        DataGetPreferredFrameTimelineIndex),
      E("AChoreographerFrameCallbackData_getFrameTimelineVsyncId", DataGetFrameTimelineVsyncId),
      E("AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos",
        DataGetFrameTimelineExpectedPresentationTimeNanos),
      E("AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos",
        DataGetFrameTimelineDeadlineNanos),
  };
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::bionic
