// gesher-base: syscall() for the Linux system calls AOSP invokes directly.
//
// futex is the important one: ART's mutexes, condition variables and
// suspend barriers are built on it. It maps onto WaitOnAddress /
// WakeByAddress*, which have the same compare-and-block semantics.
//   - FUTEX_REQUEUE / CMP_REQUEUE cannot move waiters between addresses; they
//     wake every waiter instead. Callers (condition variables) re-check their
//     condition after waking, so this is correct, just less efficient.
//   - Wake counts are not reported by Windows; FUTEX_WAKE returns the
//     requested count (callers only test for errors).

#include <sys/syscall.h>

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <windows.h>

namespace {

// Milliseconds until an absolute deadline on `clock`, rounded up.
DWORD MillisUntil(const timespec* deadline, clockid_t clock) {
  timespec now;
  clock_gettime(clock, &now);
  int64_t ns = (static_cast<int64_t>(deadline->tv_sec) - now.tv_sec) * 1'000'000'000 +
               (deadline->tv_nsec - now.tv_nsec);
  if (ns <= 0) return 0;
  int64_t ms = (ns + 999'999) / 1'000'000;
  return ms >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(ms);
}

DWORD MillisFor(const timespec* timeout) {
  int64_t ms = static_cast<int64_t>(timeout->tv_sec) * 1000 + (timeout->tv_nsec + 999'999) / 1'000'000;
  return ms >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(ms);
}

long FutexWait(volatile int* addr, int expected, DWORD millis) {
  if (*addr != expected) {
    errno = EAGAIN;
    return -1;
  }
  int compare = expected;
  if (!WaitOnAddress(const_cast<int*>(addr), &compare, sizeof(int), millis)) {
    errno = GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINTR;
    return -1;
  }
  // WaitOnAddress may return spuriously, which futex callers already handle.
  return 0;
}

long FutexWake(volatile int* addr, int count) {
  if (count <= 0) return 0;
  if (count == 1) {
    WakeByAddressSingle(const_cast<int*>(addr));
  } else {
    WakeByAddressAll(const_cast<int*>(addr));
  }
  return count;
}

long Futex(volatile int* addr, int op, int val, const timespec* timeout, volatile int* addr2,
           int val3) {
  (void)addr2;
  switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT:
      return FutexWait(addr, val, timeout == nullptr ? INFINITE : MillisFor(timeout));
    case FUTEX_WAIT_BITSET: {
      clockid_t clock = (op & FUTEX_CLOCK_REALTIME) != 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
      return FutexWait(addr, val, timeout == nullptr ? INFINITE : MillisUntil(timeout, clock));
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
      return FutexWake(addr, val);
    case FUTEX_CMP_REQUEUE:
      if (*addr != val3) {
        errno = EAGAIN;
        return -1;
      }
      [[fallthrough]];
    case FUTEX_REQUEUE:
      WakeByAddressAll(const_cast<int*>(addr));
      return val;
    default:
      errno = ENOSYS;
      return -1;
  }
}

}  // namespace

extern "C" long syscall(long number, ...) {
  va_list args;
  va_start(args, number);
  long result;
  switch (number) {
    case SYS_futex: {
      auto addr = va_arg(args, volatile int*);
      int op = va_arg(args, int);
      int val = va_arg(args, int);
      auto timeout = va_arg(args, const timespec*);
      auto addr2 = va_arg(args, volatile int*);
      int val3 = va_arg(args, int);
      result = Futex(addr, op, val, timeout, addr2, val3);
      break;
    }
    case SYS_gettid:
      result = static_cast<long>(GetCurrentThreadId());
      break;
    case SYS_getpid:
      result = static_cast<long>(GetCurrentProcessId());
      break;
    default:
      errno = ENOSYS;
      result = -1;
      break;
  }
  va_end(args);
  return result;
}
