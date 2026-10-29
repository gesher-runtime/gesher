// gesher-base: <pthread.h> additions.

#include <pthread.h>

#include <errno.h>
#include <time.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include <climits>
#include <cstdint>

#if GESHER_WINPTHREAD_CPU_AUTHORITY_API != 1
#error Rebuild the pinned Gesher winpthreads authority supplier before building hosts
#endif

// Calls to the supplier's fixed clocks must bypass the public overlay.
#undef clock_gettime
#undef clock_getres

#include "internal.h"

// The real winpthreads calls; <pthread.h> routes everyone else here.
#undef pthread_key_create
#undef pthread_key_delete

namespace {

// Key destructors, each behind a trampoline that skips NULL values, as
// POSIX specifies; winpthreads calls a destructor for every key that was
// set. Keys with destructors are few (ART has a handful), so a fixed table
// of trampolines does.
constexpr int kSlots = 128;
std::atomic<void (*)(void*)> slot_destructor[kSlots];
pthread_key_t slot_key[kSlots];
std::mutex slot_lock;

template <int I>
void Trampoline(void* value) {
  void (*destructor)(void*) = slot_destructor[I].load(std::memory_order_acquire);
  if (value != nullptr && destructor != nullptr) destructor(value);
}

template <int... I>
constexpr auto MakeTrampolines(std::integer_sequence<int, I...>) {
  return std::array<void (*)(void*), sizeof...(I)>{&Trampoline<I>...};
}
constexpr auto kTrampolines = MakeTrampolines(std::make_integer_sequence<int, kSlots>());

struct TargetClock {
  HANDLE handle = nullptr;
  unsigned long long generation = 0;
  clockid_t id = 0;
};
std::mutex cpu_clock_lock;
std::array<TargetClock, 256> cpu_clocks;
// Never reissue an ID, even when its slot is reclaimed.
int64_t next_cpu_clock = -1073741824;

void ReapTargetClocks() {
  for (auto& clock : cpu_clocks) {
    if (clock.handle && WaitForSingleObject(clock.handle, 0) == WAIT_OBJECT_0) {
      CloseHandle(clock.handle);
      clock = {};
    }
  }
}

int ReadTargetClock(clockid_t id, timespec* value, bool resolution) {
  std::lock_guard<std::mutex> guard(cpu_clock_lock);
  ReapTargetClocks();
  for (const auto& clock : cpu_clocks) {
    if (!clock.handle || clock.id != id) continue;
    if (resolution) {
      if (value) *value = {0, 100};
      return 0;
    }
    if (!value) { errno = EFAULT; return -1; }
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(clock.handle, &created, &exited, &kernel, &user)) break;
    auto ticks = [](FILETIME t) {
      return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    uint64_t total = ticks(kernel) + ticks(user);
    timespec result{static_cast<time_t>(total / 10000000),
                    static_cast<long>((total % 10000000) * 100)};
    *value = result;
    return 0;
  }
  errno = EINVAL;
  return -1;
}

}  // namespace

extern "C" {

int gesher_pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
  if (destructor == nullptr) return pthread_key_create(key, nullptr);
  std::lock_guard<std::mutex> guard(slot_lock);
  for (int i = 0; i < kSlots; ++i) {
    if (slot_destructor[i].load(std::memory_order_relaxed) != nullptr) continue;
    int rc = pthread_key_create(key, kTrampolines[i]);
    if (rc != 0) return rc;
    slot_key[i] = *key;
    slot_destructor[i].store(destructor, std::memory_order_release);
    return 0;
  }
  return EAGAIN;  // as when the system is out of keys
}

int gesher_pthread_key_delete(pthread_key_t key) {
  int rc = pthread_key_delete(key);
  if (rc != 0) return rc;
  std::lock_guard<std::mutex> guard(slot_lock);
  for (int i = 0; i < kSlots; ++i) {
    if (slot_destructor[i].load(std::memory_order_relaxed) != nullptr && slot_key[i] == key) {
      slot_destructor[i].store(nullptr, std::memory_order_release);
      break;
    }
  }
  return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr) {
  if (!pthread_equal(thread, pthread_self())) return ENOTSUP;
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  int rc = pthread_attr_init(attr);
  if (rc != 0) return rc;
  return pthread_attr_setstack(attr, reinterpret_cast<void*>(low), high - low);
}

int pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id) {
  if (!clock_id) return EINVAL;
  // Ensure a Win32-created calling thread has its genuine supplier identity.
  (void)pthread_self();
  void* owned = nullptr;
  unsigned long long generation = 0;
  int result = gesher_winpthread_dup_cpu_handle_v1(thread, &owned, &generation);
  if (result) return gesher_errno_from_crt(result);
  std::lock_guard<std::mutex> guard(cpu_clock_lock);
  // A supplier duplicate may have waited behind a query while its target
  // exited. Reject it before a reaped saved clock can be minted a new ID.
  if (WaitForSingleObject(owned, 0) != WAIT_TIMEOUT) {
    CloseHandle(owned);
    ReapTargetClocks();
    return ESRCH;
  }
  for (const auto& clock : cpu_clocks) {
    if (clock.handle && clock.generation == generation) {
      CloseHandle(owned);
      *clock_id = clock.id;
      return 0;
    }
  }
  ReapTargetClocks();
  if (WaitForSingleObject(owned, 0) != WAIT_TIMEOUT) {
    CloseHandle(owned);
    return ESRCH;
  }
  if (next_cpu_clock >= INT_MIN) {
    for (auto& clock : cpu_clocks) {
      if (clock.handle) continue;
      clock = {owned, generation, static_cast<clockid_t>(next_cpu_clock--)};
      *clock_id = clock.id;
      return 0;
    }
  }
  CloseHandle(owned);
  return EAGAIN;
}

int gesher_clock_gettime(clockid_t clock, timespec* value) {
  if (clock <= -1073741824) return ReadTargetClock(clock, value, false);
  return clock_gettime(clock, value);
}

int gesher_clock_getres(clockid_t clock, timespec* value) {
  if (clock <= -1073741824) return ReadTargetClock(clock, value, true);
  return clock_getres(clock, value);
}

// The bottom of the stack Windows keeps (gesher::StackBottomReserve): ART
// puts its protected region for implicit stack overflow checks right above
// what this reports (fault_signals.cc).
int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize) {
  (void)attr;
  *guardsize = gesher::StackBottomReserve();
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t* attr, size_t guardsize) {
  (void)attr;
  (void)guardsize;
  return 0;
}

int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
  (void)prepare;
  (void)parent;
  (void)child;
  return 0;
}

}  // extern "C"
