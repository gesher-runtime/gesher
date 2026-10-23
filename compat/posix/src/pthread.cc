// gesher-base: <pthread.h> additions.

#include <pthread.h>

#include <errno.h>
#include <time.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

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
  (void)thread;
  *clock_id = CLOCK_THREAD_CPUTIME_ID;
  return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize) {
  (void)attr;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  *guardsize = si.dwPageSize;
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
