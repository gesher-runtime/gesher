// Shared C/C++ Android CLOCK_MONOTONIC source. Unlike the host QPC clock,
// unbiased interrupt time excludes Windows sleep and hibernate.
#pragma once
#include <stdint.h>
#include <windows.h>

static inline int64_t gesher_monotonic_nanos(void) {
  ULONGLONG ticks;
  QueryUnbiasedInterruptTimePrecise(&ticks);
  return ticks > (ULONGLONG)(INT64_MAX / 100) ? INT64_MAX : (int64_t)ticks * 100;
}
