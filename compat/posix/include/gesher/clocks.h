// Android elapsed-time domains on Windows. Keep these separate from the host
// winpthreads clock: its absolute condition-variable deadlines use its own QPC
// epoch. Android/ART callers explicitly opt into these clocks instead.
#pragma once

#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <windows.h>
#include <gesher/awake_clock.h>

namespace gesher {

inline int64_t InterruptTicksToNanos(ULONGLONG ticks) {
  // Win32 interrupt clocks count 100 ns units. Saturate before conversion.
  return ticks > static_cast<ULONGLONG>(INT64_MAX / 100)
             ? INT64_MAX : static_cast<int64_t>(ticks) * 100;
}

// Linux CLOCK_MONOTONIC / Android uptime: time spent asleep is excluded.
inline int64_t MonotonicNanos() {
  return gesher_monotonic_nanos();
}

// Linux CLOCK_BOOTTIME / Android elapsedRealtime: sleep/hibernate is included.
inline int64_t BoottimeNanos() {
  ULONGLONG ticks;
  QueryInterruptTimePrecise(&ticks);
  return InterruptTicksToNanos(ticks);
}

// Host clock IDs are used here, not the guest Linux ABI's integer IDs. Wall
// and CPU clocks retain winpthreads' implementation and validation.
inline int AndroidClockGettime(clockid_t clock, struct timespec* value) {
  if (clock != CLOCK_MONOTONIC) return clock_gettime(clock, value);
  if (value == nullptr) return errno = EFAULT, -1;
  const int64_t nanos = MonotonicNanos();
  value->tv_sec = nanos / 1000000000;
  value->tv_nsec = static_cast<long>(nanos % 1000000000);
  return 0;
}

}  // namespace gesher
