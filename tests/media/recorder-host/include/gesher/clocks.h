#pragma once

#include <time.h>

// The host has a native monotonic clock. The Recorder is compiled unchanged.
namespace gesher {
inline int AndroidClockGettime(clockid_t clock, timespec* result) {
  return clock_gettime(clock, result);
}
}
