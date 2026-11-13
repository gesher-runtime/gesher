// Linux LP64 timerfd values, shared by the host backend and Android ABI adapters.
#pragma once

#include <stdint.h>

namespace gesher {

struct TimerFdTime {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct TimerFdSpec {
  TimerFdTime it_interval;
  TimerFdTime it_value;
};
static_assert(sizeof(TimerFdSpec) == 32);

// Arguments and errno use Linux values. These are C++ host entrypoints; bionic
// supplies the guest calling convention, and does not own descriptor state.
int TimerFdCreate(int clockid, int flags);
int TimerFdSetTime(int fd, int flags, const TimerFdSpec* value, TimerFdSpec* previous);
int TimerFdGetTime(int fd, TimerFdSpec* value);

}  // namespace gesher
