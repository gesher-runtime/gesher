// Linux syscall contracts used by AOSP. Futex queues belong to the executable
// so independently linked PE modules and guest libc share the same waiters.

#include <sys/syscall.h>
#include <sys/mman.h>

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <windows.h>

#include <atomic>

namespace {
struct Waiter {
  volatile int* address;
  uint32_t mask;
  bool private_key;
  Waiter* next = nullptr;
  Waiter* previous = nullptr;
  std::atomic<int> ready{0};
};
static_assert(sizeof(std::atomic<int>) == sizeof(int) && std::atomic<int>::is_always_lock_free);
struct Bucket { Waiter* first = nullptr; Waiter* last = nullptr; };
SRWLOCK g_futex_lock = SRWLOCK_INIT;
Bucket g_futex_buckets[64];
Bucket& BucketFor(const volatile int* address) {
  uintptr_t key = reinterpret_cast<uintptr_t>(address) >> 2;
  return g_futex_buckets[(key ^ (key >> 7)) % 64];
}
void Append(Waiter* waiter) {
  Bucket& b = BucketFor(waiter->address);
  waiter->previous = b.last; waiter->next = nullptr;
  if (b.last) b.last->next = waiter; else b.first = waiter;
  b.last = waiter;
}
void Remove(Waiter* waiter) {
  Bucket& b = BucketFor(waiter->address);
  if (waiter->previous) waiter->previous->next = waiter->next; else b.first = waiter->next;
  if (waiter->next) waiter->next->previous = waiter->previous; else b.last = waiter->previous;
}
bool Aligned(const volatile int* address) {
  return (reinterpret_cast<uintptr_t>(address) & 3) == 0;
}
struct Deadline {
  bool timed = false;
  clockid_t clock = CLOCK_MONOTONIC;
  __int128 nanoseconds = 0;
};
bool SetDeadline(const timespec* time, int command, bool realtime, Deadline* out) {
  if (!time) return true;
  if (time->tv_sec < 0 || time->tv_nsec < 0 || time->tv_nsec >= 1000000000)
    return errno = EINVAL, false;
  out->timed = true;
  out->clock = realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  out->nanoseconds = static_cast<__int128>(time->tv_sec) * 1000000000 + time->tv_nsec;
  if (command == FUTEX_WAIT) {
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    out->nanoseconds += static_cast<__int128>(now.tv_sec) * 1000000000 + now.tv_nsec;
  }
  return true;
}
DWORD Remaining(const Deadline& deadline) {
  if (!deadline.timed) return INFINITE;
  timespec now;
  if (clock_gettime(deadline.clock, &now) != 0) return 0;
  __int128 remaining = deadline.nanoseconds -
      (static_cast<__int128>(now.tv_sec) * 1000000000 + now.tv_nsec);
  if (remaining <= 0) return 0;
  __int128 millis = (remaining + 999999) / 1000000;
  DWORD cap = deadline.clock == CLOCK_REALTIME ? 1000 : INFINITE - 1;
  return millis > cap ? cap : static_cast<DWORD>(millis);
}
long Wait(volatile int* address, int value, uint32_t mask, bool private_key,
          const Deadline& deadline) {
  Waiter waiter{address, mask, private_key};
  AcquireSRWLockExclusive(&g_futex_lock);
  if (__atomic_load_n(address, __ATOMIC_SEQ_CST) != value) {
    ReleaseSRWLockExclusive(&g_futex_lock);
    return errno = EAGAIN, -1;
  }
  Append(&waiter);
  ReleaseSRWLockExclusive(&g_futex_lock);
  int result = 0, error = 0;
  for (;;) {
    DWORD millis = Remaining(deadline);
    if (waiter.ready.load(std::memory_order_acquire)) break;
    if (millis == 0) { result = -1; error = ETIMEDOUT; break; }
    int compare = 0;
    if (!WaitOnAddress(&waiter.ready, &compare, sizeof(compare), millis)) {
      DWORD last_error = GetLastError();
      if (last_error != ERROR_TIMEOUT) { result = -1; error = EIO; break; }
    }
    // Timeouts are slices (and address waits may wake spuriously). Recompute
    // against the original full-width deadline, including realtime clock steps.
  }
  AcquireSRWLockExclusive(&g_futex_lock);
  if (waiter.ready.load(std::memory_order_relaxed)) result = 0;
  else Remove(&waiter);
  // Wakers signal while holding this lock, so none can access this stack node
  // after the waiting thread releases the lock and returns.
  ReleaseSRWLockExclusive(&g_futex_lock);
  if (result != 0) errno = error;
  return result;
}
long WakeOrRequeue(volatile int* address, int command, int count, uint32_t mask,
                   bool private_key, volatile int* target, int requeue_count, int compare) {
  AcquireSRWLockExclusive(&g_futex_lock);
  if (command == FUTEX_CMP_REQUEUE && __atomic_load_n(address, __ATOMIC_SEQ_CST) != compare) {
    ReleaseSRWLockExclusive(&g_futex_lock);
    return errno = EAGAIN, -1;
  }
  bool requeue = command == FUTEX_REQUEUE || command == FUTEX_CMP_REQUEUE;
  // Linux's non-PI wake loop can wake one waiter even for a nonpositive count.
  int wake_limit = !requeue && count <= 0 ? 1 : count;
  int woken = 0, moved = 0;
  for (Waiter* node = BucketFor(address).first; node;) {
    Waiter* next = node->next;
    if (node->address == address && node->private_key == private_key && (node->mask & mask)) {
      if (woken < wake_limit) {
        Remove(node);
        node->ready.store(1, std::memory_order_release);
        WakeByAddressSingle(&node->ready);
        ++woken;
      } else if (requeue && moved < requeue_count) {
        if (address != target) { Remove(node); node->address = target; Append(node); }
        ++moved;
      } else break;
    }
    node = next;
  }
  ReleaseSRWLockExclusive(&g_futex_lock);
  return static_cast<long>(woken) + moved;
}
long FutexOwned(volatile int* address, int op, int value, const timespec* time,
                volatile int* target, int value3) {
  int command = op & FUTEX_CMD_MASK;
  bool realtime = (op & FUTEX_CLOCK_REALTIME) != 0;
  bool private_key = (op & FUTEX_PRIVATE_FLAG) != 0;
  bool wait = command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET;
  Deadline deadline;
  if (wait && !SetDeadline(time, command, realtime, &deadline)) return -1;
  if (realtime && command != FUTEX_WAIT_BITSET) return errno = ENOSYS, -1;
  if (command != FUTEX_WAIT && command != FUTEX_WAIT_BITSET && command != FUTEX_WAKE &&
      command != FUTEX_WAKE_BITSET && command != FUTEX_REQUEUE && command != FUTEX_CMP_REQUEUE)
    return errno = ENOSYS, -1;
  uint32_t mask = (command == FUTEX_WAIT_BITSET || command == FUTEX_WAKE_BITSET)
                      ? static_cast<uint32_t>(value3) : UINT32_MAX;
  if (mask == 0 || !Aligned(address)) return errno = EINVAL, -1;
  if (!address && (wait || !private_key || command == FUTEX_CMP_REQUEUE)) return errno = EFAULT, -1;
  if (wait) return Wait(address, value, mask, private_key, deadline);
  int requeue_count = 0;
  if (command == FUTEX_REQUEUE || command == FUTEX_CMP_REQUEUE) {
    requeue_count = static_cast<int>(reinterpret_cast<intptr_t>(time));
    if (value < 0 || requeue_count < 0 || !Aligned(target)) return errno = EINVAL, -1;
    if (!target && !private_key) return errno = EFAULT, -1;
  }
  return WakeOrRequeue(address, command, value, mask, private_key, target, requeue_count, value3);
}
using FutexOwner = long (*)(volatile int*, int, int, const timespec*, volatile int*, int);
long Futex(volatile int* address, int op, int value, const timespec* time,
           volatile int* target, int value3) {
  // Never fall back to this DLL's copy: that would partition waiters silently.
  static auto owner = reinterpret_cast<FutexOwner>(
      GetProcAddress(GetModuleHandleW(nullptr), "gesher_futex_owner_v1"));
  if (!owner) return errno = ENOSYS, -1;
  return owner(address, op, value, time, target, value3);
}
}  // namespace

// Link the private anchor, not the public export: an import library can satisfy
// -u on the public name without extracting the executable's archive member.
extern "C" void gesher_futex_owner_anchor_v1() {}
extern "C" __declspec(dllexport) long gesher_futex_owner_v1(
    volatile int* address, int op, int value, const timespec* time, volatile int* target, int value3) {
  return FutexOwned(address, op, value, time, target, value3);
}

extern "C" long syscall(long number, ...) {
  va_list args;
  va_start(args, number);
  long result;
  switch (number) {
    case SYS_memfd_create: {
      const char* name = va_arg(args, const char*);
      unsigned int flags = va_arg(args, unsigned int);
      result = memfd_create(name, flags);
      break;
    }
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
