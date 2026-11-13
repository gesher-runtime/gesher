// timerfd: one shared open description, with read-only eventfd readiness.
//
// Descriptor aliases belong to epoll.cc's object registry. Neither callbacks
// nor this object retain a numeric fd. Final release cancels and joins the
// Windows timer outside that registry's lock. Rearming uses the same join,
// since disabling a threadpool timer alone does not revoke queued callbacks.
#include <gesher/timerfd.h>

#include <errno.h>
#include <string.h>
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>

#include "event_internal.h"
#include "internal.h"
#include "record_lock_descriptors.h"

namespace gesher {
namespace {

constexpr int kRealtime = 0;
constexpr int kMonotonic = 1;
constexpr int kBoottime = 7;
constexpr int kNonblock = 04000;
constexpr int kCloexec = 02000000;
constexpr int kAbstime = 1;
constexpr int kCancelOnSet = 2;
constexpr int64_t kNanos = 1000000000;
constexpr int64_t kMaxTime = std::numeric_limits<int64_t>::max();
constexpr int64_t kUnixEpoch100ns = 116444736000000000LL;

bool ValidTime(TimerFdTime time) {
  return time.tv_sec >= 0 && time.tv_nsec >= 0 && time.tv_nsec < kNanos;
}

// Linux timespec64_to_ktime saturates large valid values to KTIME_MAX.
int64_t ToNanos(TimerFdTime time) {
  if (time.tv_sec >= kMaxTime / kNanos) return kMaxTime;
  const __int128 value = static_cast<__int128>(time.tv_sec) * kNanos + time.tv_nsec;
  return value > kMaxTime ? kMaxTime : static_cast<int64_t>(value);
}

TimerFdTime FromNanos(int64_t value) {
  return {value / kNanos, value % kNanos};
}

int64_t MonotonicNanos() {
  // Keep the same origin as bionic's clock_gettime, including CLOCK_BOOTTIME.
  // Separate suspend-aware clock qualification remains necessary.
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  const __int128 nanos = static_cast<__int128>(value.QuadPart) * kNanos / frequency.QuadPart;
  return nanos > kMaxTime ? kMaxTime : static_cast<int64_t>(nanos);
}

int64_t RealtimeNanos() {
  FILETIME value;
  GetSystemTimePreciseAsFileTime(&value);
  const uint64_t ticks = static_cast<uint64_t>(value.dwHighDateTime) << 32 |
                         value.dwLowDateTime;
  const __int128 nanos = (static_cast<__int128>(ticks) - kUnixEpoch100ns) * 100;
  return static_cast<int64_t>(std::clamp(nanos, -static_cast<__int128>(kMaxTime),
                                       static_cast<__int128>(kMaxTime)));
}

}  // namespace

struct TimerFd {
  // Settime and destruction serialize the disable/join/reprogram transaction.
  // Callbacks never take this lock, the descriptor transaction, or registry lock.
  std::mutex operation;
  std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<EventFd> readiness;
  PTP_TIMER timer = nullptr;
  int clockid = kMonotonic;
  bool changing = false;
  bool absolute_realtime = false;
  int64_t next = 0;
  int64_t interval = 0;
  uint64_t pending = 0;

  ~TimerFd() {
    if (timer == nullptr) return;
    std::lock_guard<std::mutex> serial(operation);
    Quiesce();
    CloseThreadpoolTimer(timer);
  }

  void Quiesce() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      changing = true;
    }
    SetThreadpoolTimer(timer, nullptr, 0, 0);
    // Never wait with mutex held: an already running callback needs it to exit.
    WaitForThreadpoolTimerCallbacks(timer, TRUE);
  }

  int64_t Now() const { return absolute_realtime ? RealtimeNanos() : MonotonicNanos(); }

  // Account for all elapsed periods, even when Windows delayed/coalesced a
  // callback. Windows' millisecond period is not used as the source of truth.
  void Expire(int64_t now) {
    if (next == 0 || now < next) return;
    uint64_t count = 1;
    if (interval > 0) {
      count += static_cast<uint64_t>((static_cast<__int128>(now) - next) / interval);
      const __int128 following = static_cast<__int128>(next) +
                                 static_cast<__int128>(count) * interval;
      next = following > kMaxTime ? kMaxTime : static_cast<int64_t>(following);
    } else {
      next = 0;
    }
    const uint64_t room = std::numeric_limits<uint64_t>::max() - pending;
    pending += std::min(count, room);
    SetReadable(*readiness, true);
    changed.notify_all();
  }

  TimerFdSpec Snapshot(int64_t now) {
    Expire(now);
    // Linux reports the next periodic deadline after forwarding expired ticks.
    const __int128 left = next == 0 ? 0 : static_cast<__int128>(next) - now;
    const int64_t remaining = left <= 0 ? 0 : left > kMaxTime ? kMaxTime : static_cast<int64_t>(left);
    return {FromNanos(interval), FromNanos(remaining)};
  }

  void Arm() {
    if (next == 0 || changing) return;
    int64_t ticks;
    if (absolute_realtime) {
      // Absolute FILETIME timers track Windows wall-clock adjustments.
      ticks = kUnixEpoch100ns + next / 100 + (next % 100 != 0);
    } else {
      const __int128 left = static_cast<__int128>(next) - Now();
      const __int128 rounded = left > 0 ? (left + 99) / 100 : 1;
      ticks = -static_cast<int64_t>(std::min(rounded, static_cast<__int128>(kMaxTime)));
    }
    const uint64_t bits = static_cast<uint64_t>(ticks);
    FILETIME due{static_cast<DWORD>(bits), static_cast<DWORD>(bits >> 32)};
    SetThreadpoolTimer(timer, &due, 0, 0);
  }
};

namespace {

VOID CALLBACK Expired(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
  auto& timer = *static_cast<TimerFd*>(context);
  std::lock_guard<std::mutex> lock(timer.mutex);
  if (timer.changing) return;
  timer.Expire(timer.Now());
  timer.Arm();
}

std::shared_ptr<TimerFd> CheckedTimer(int fd) {
  record_locks::DescriptorGuard descriptors;
  if (HandleFromFd(fd) == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return nullptr;
  }
  auto timer = LookupTimerFd(fd);
  if (timer == nullptr) errno = EINVAL;
  return timer;
}

}  // namespace

int TimerFdCreate(int clockid, int flags) {
  if ((clockid != kRealtime && clockid != kMonotonic && clockid != kBoottime) ||
      (flags & ~(kNonblock | kCloexec)) != 0) return errno = EINVAL, -1;
  auto timer = std::make_shared<TimerFd>();
  timer->clockid = clockid;
  timer->readiness = NewReadiness((flags & kNonblock) != 0);
  if (timer->readiness == nullptr) return FailWithLastError();
  timer->timer = CreateThreadpoolTimer(Expired, timer.get(), nullptr);
  if (timer->timer == nullptr) return FailWithLastError();
  auto readiness = timer->readiness;
  // ReserveFd is always non-inheritable, just as for eventfd and inotify.
  record_locks::DescriptorGuard descriptors;
  return RegisterTimerFd(std::move(timer), std::move(readiness));
}

int TimerFdGetTime(int fd, TimerFdSpec* value) {
  auto timer = CheckedTimer(fd);
  if (timer == nullptr) return -1;
  if (value == nullptr) return errno = EFAULT, -1;
  std::lock_guard<std::mutex> lock(timer->mutex);
  *value = timer->Snapshot(timer->Now());
  return 0;
}

int TimerFdSetTime(int fd, int flags, const TimerFdSpec* value, TimerFdSpec* previous) {
  if (value == nullptr) return errno = EFAULT, -1;
  const TimerFdSpec requested = *value;  // old/new pointers may alias
  if ((flags & ~(kAbstime | kCancelOnSet)) != 0 ||
      !ValidTime(requested.it_value) || !ValidTime(requested.it_interval)) {
    return errno = EINVAL, -1;
  }
  auto timer = CheckedTimer(fd);
  if (timer == nullptr) return -1;
  // CANCEL_ON_SET has no effect on relative or non-realtime timers. Windows
  // supplies no timer-local equivalent for Linux's realtime clock-set cancel
  // notification; do not accept that combination and silently lose ECANCELED.
  if (timer->clockid == kRealtime && (flags & (kAbstime | kCancelOnSet)) ==
                                        (kAbstime | kCancelOnSet)) {
    return errno = EOPNOTSUPP, -1;
  }
  std::lock_guard<std::mutex> serial(timer->operation);
  timer->Quiesce();
  std::lock_guard<std::mutex> lock(timer->mutex);
  if (previous != nullptr) *previous = timer->Snapshot(timer->Now());
  timer->pending = 0;
  SetReadable(*timer->readiness, false);
  timer->interval = ToNanos(requested.it_interval);
  timer->absolute_realtime = timer->clockid == kRealtime && (flags & kAbstime) != 0;
  const int64_t value_ns = ToNanos(requested.it_value);
  const int64_t now = timer->Now();
  if (value_ns == 0) {
    timer->next = 0;
  } else if ((flags & kAbstime) != 0) {
    timer->next = value_ns;
  } else {
    const __int128 target = static_cast<__int128>(now) + value_ns;
    timer->next = target > kMaxTime ? kMaxTime : static_cast<int64_t>(target);
  }
  timer->changing = false;
  timer->Expire(now);
  timer->Arm();
  return 0;
}

int ReadTimerFd(TimerFd& timer, void* buffer, unsigned int size) {
  if (size < sizeof(uint64_t)) return errno = EINVAL, -1;
  if (buffer == nullptr) return errno = EFAULT, -1;
  std::unique_lock<std::mutex> lock(timer.mutex);
  for (;;) {
    timer.Expire(timer.Now());
    if (timer.pending != 0) break;
    if (IsNonBlocking(*timer.readiness)) return errno = EAGAIN, -1;
    timer.changed.wait(lock);
  }
  const uint64_t value = timer.pending;
  timer.pending = 0;
  SetReadable(*timer.readiness, false);
  memcpy(buffer, &value, sizeof(value));
  return sizeof(value);
}

}  // namespace gesher
