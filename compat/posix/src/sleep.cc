// gesher-base: nanosleep that never returns before the time asked for.
//
// winpthreads' nanosleep waits with Sleep, whose expiry is counted in
// scheduler ticks: it can return up to a tick (15.6 ms by default) before
// the precise clocks (QueryPerformanceCounter, GetSystemTimePreciseAsFileTime)
// show the interval as passed. Linux sleeps at least the time requested, and
// code compares the two (libutils' SystemClock_test). <pthread_time.h>
// inlines nanosleep as a call to nanosleep64, which is defined here; being
// linked before winpthreads, this one is the one every module gets.
#include <errno.h>
#include <time.h>
#include <windows.h>

#include "internal.h"

extern "C" int nanosleep64(const struct _timespec64* request, struct _timespec64* remain) {
  if (request == nullptr) return errno = EFAULT, -1;
  if (request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L) {
    return errno = EINVAL, -1;
  }
  constexpr long long kMaxSeconds = 0x7fffffffffffffffLL / 1000000 - 1;
  long long seconds = request->tv_sec < kMaxSeconds ? request->tv_sec : kMaxSeconds;
  ULONGLONG deadline = gesher::MonotonicMicros() + static_cast<ULONGLONG>(seconds) * 1000000 +
                       static_cast<ULONGLONG>((request->tv_nsec + 999) / 1000);
  for (;;) {
    ULONGLONG now = gesher::MonotonicMicros();
    if (now >= deadline) break;
    ULONGLONG ms = (deadline - now + 999) / 1000;
    Sleep(ms >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(ms));
  }
  if (remain != nullptr) {
    remain->tv_sec = 0;
    remain->tv_nsec = 0;
  }
  return 0;
}
