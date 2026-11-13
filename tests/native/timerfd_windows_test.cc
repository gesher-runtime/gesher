// Actual Windows TimerFd backend and CRT descriptor routes. The shared fixture
// uses this small typed dispatch adapter instead of a guest calling convention;
// libc_test exercises actual numeric syscalls separately in both Android ABIs.
#include <gesher/timerfd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>

static int failures, assertions;
static void check(const char* name, int success) {
  ++assertions;
  if (!success) ++failures;
  std::printf("timerfd-host: %s %s\n", success ? "PASS" : "FAIL", name);
}

template<class T> static int64_t TimerArgument(T value) {
  if constexpr (std::is_pointer_v<T>) return reinterpret_cast<int64_t>(value);
  else return static_cast<int64_t>(value);
}
template<class... Args> static int64_t TimerCall(int number, Args... arguments) {
  const std::array<int64_t, 4> values{TimerArgument(arguments)...};
  switch (number) {
    case 283: return gesher::TimerFdCreate(static_cast<int>(values[0]), static_cast<int>(values[1]));
    case 286: return gesher::TimerFdSetTime(static_cast<int>(values[0]), static_cast<int>(values[1]),
                    reinterpret_cast<const gesher::TimerFdSpec*>(values[2]),
                    reinterpret_cast<gesher::TimerFdSpec*>(values[3]));
    case 287: return gesher::TimerFdGetTime(static_cast<int>(values[0]),
                    reinterpret_cast<gesher::TimerFdSpec*>(values[1]));
    default: return -1;  // the host fixture only dispatches the three typed APIs
  }
}

#define GESHER_TIMERFD_HOST 1
#define _Static_assert static_assert
#define syscall TimerCall
#define itimerspec gesher::TimerFdSpec
#define SYS_timerfd_create 283
#define SYS_timerfd_settime 286
#define SYS_timerfd_gettime 287
#define TFD_NONBLOCK 04000
#define TFD_CLOEXEC 02000000
#define TFD_TIMER_CANCEL_ON_SET 2
#undef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#undef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#include "timerfd_syscall_test.inc"

int main() {
  test_timerfd_syscalls();
  std::printf("timerfd-host: %d assertions, %d failures\n", assertions, failures);
  return failures ? 1 : 0;
}
