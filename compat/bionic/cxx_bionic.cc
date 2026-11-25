// gesher bionic shim: libstdc++.so, bionic's minimal C++ runtime support
// library (bionic/libc/libstdc++.map.txt; docs/native-libs.md).
//
// On every Android device /system/lib64/libstdc++.so exports exactly these
// LP64 symbols, built from bionic/libc/bionic/{new.cpp,__cxa_guard.cpp,
// __cxa_pure_virtual.cpp}:
//   _ZSt7nothrow                     std::nothrow
//   _Znwm, _Znam                     operator new / new[](size_t)
//   _ZnwmRKSt9nothrow_t, _ZnamRKSt9nothrow_t   their nothrow forms
//   _ZdlPv, _ZdaPv                   operator delete / delete[](void*)
//   _ZdlPvRKSt9nothrow_t, _ZdaPvRKSt9nothrow_t their nothrow forms
//   __cxa_guard_acquire/_release/_abort
//   __cxa_pure_virtual
// (new.cpp also defines the sized deletes, but the version script keeps them
// local, so they are not exported and not registered here.)
//
// Semantics are bionic's: operator new allocates with libc's malloc, so its
// memory may be released with free() and vice versa; the throwing forms do not
// throw (bionic has no exceptions in libc) but end the process the way
// async_safe_fatal does; the nothrow forms return malloc's result, null on
// failure. The guards use bionic's 32-bit state word in the first four bytes
// of the 64-bit guard variable, with the same values, so the compiler's inline
// fast path (first byte non-zero, or bit 0 on arm64) sees the same states.
//
// The functions only take integers and pointers and touch no host-specific
// state, so an arm64 guest calls these same System V entry points through the
// translator's host-call trampolines (compat/arm64/dll/guest_libc.cc needs no
// guest-specific form). Loaded ELF objects are searched before the shim, so an
// app's own libc++_shared.so (or a static libc++) keeps its definitions.

#include "shim_internal.h"

#include <gesher/bionic_shim.h>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace gesher::bionic {

namespace {

// The current table entry for a libc function (later registrations, such as
// signal_bionic.cc's abort, replace earlier ones), resolved on first use.
template <typename Fn>
Fn Libc(const char* name) {
  return reinterpret_cast<Fn>(LookupBionicSymbol(name));
}

using MallocFn = void*(BIONIC_SYSV*)(size_t);
using FreeFn = void(BIONIC_SYSV*)(void*);
using LogWriteFn = int(BIONIC_SYSV*)(int, const char*, const char*);
using AbortMessageFn = void(BIONIC_SYSV*)(const char*);
using AbortFn = void(BIONIC_SYSV*)();

void* Allocate(size_t size) {
  static const MallocFn malloc_fn = Libc<MallocFn>("malloc");
  return malloc_fn(size);
}
void Release(void* p) {
  static const FreeFn free_fn = Libc<FreeFn>("free");
  free_fn(p);
}

// async_safe_fatal: the message to stderr and to the log (tag "libc",
// ANDROID_LOG_FATAL), recorded as the abort message, then abort(), which runs a
// SIGABRT handler the program installed (signal_bionic.cc).
[[noreturn]] void Fatal(const char* msg) {
  std::fprintf(stderr, "%s\n", msg);
  std::fflush(stderr);
  if (auto log = Libc<LogWriteFn>("__android_log_write")) log(7 /* ANDROID_LOG_FATAL */, "libc", msg);
  if (auto set = Libc<AbortMessageFn>("android_set_abort_message")) set(msg);
  if (auto abort_fn = Libc<AbortFn>("abort")) abort_fn();
  std::abort();
}

[[noreturn]] void AllocationFailed(const char* what, size_t size) {
  char msg[96];
  std::snprintf(msg, sizeof(msg), "%s failed to allocate %zu bytes", what, size);
  Fatal(msg);
}

// --- new.cpp --------------------------------------------------------------------------

// std::nothrow (std::nothrow_t is an empty struct, one byte).
const unsigned char g_nothrow = 0;

void* BIONIC_SYSV New(size_t size) {
  void* p = Allocate(size);
  if (p == nullptr) AllocationFailed("new", size);
  return p;
}
void* BIONIC_SYSV NewArray(size_t size) {
  void* p = Allocate(size);
  if (p == nullptr) AllocationFailed("new[]", size);
  return p;
}
void* BIONIC_SYSV NewNothrow(size_t size, const void* /*nothrow*/) { return Allocate(size); }
void BIONIC_SYSV Delete(void* p) { Release(p); }
void BIONIC_SYSV DeleteNothrow(void* p, const void* /*nothrow*/) { Release(p); }

// --- __cxa_guard.cpp ------------------------------------------------------------------

// The state word: bionic's _guard_t is a union of an atomic_int with the
// ABI's 64-bit guard, so the state occupies its first four bytes.
constexpr int32_t kNotYetStarted = 0;
constexpr int32_t kComplete = 1;
constexpr int32_t kUnderwayWithoutWaiter = 0x100;
constexpr int32_t kUnderwayWithWaiter = 0x200;

using GuardState = std::atomic<int32_t>;
static_assert(sizeof(GuardState) == 4 && GuardState::is_always_lock_free);

// __futex_wait_ex(&state, false, kUnderwayWithWaiter): sleeps only while the
// word still holds that value; a wake or a spurious return re-reads it.
void WaitWhileWaiter(GuardState* state) {
  int32_t expected = kUnderwayWithWaiter;
  WaitOnAddress(reinterpret_cast<volatile void*>(state), &expected, sizeof(expected), INFINITE);
}

int BIONIC_SYSV GuardAcquire(void* guard) {
  auto* state = static_cast<GuardState*>(guard);
  int32_t old_value = state->load(std::memory_order_acquire);
  for (;;) {
    if (old_value == kComplete) return 0;
    if (old_value == kNotYetStarted) {
      if (!state->compare_exchange_weak(old_value, kUnderwayWithoutWaiter, std::memory_order_acquire,
                                        std::memory_order_acquire)) {
        continue;
      }
      return 1;
    }
    if (old_value == kUnderwayWithoutWaiter) {
      if (!state->compare_exchange_weak(old_value, kUnderwayWithWaiter, std::memory_order_acquire,
                                        std::memory_order_acquire)) {
        continue;
      }
    }
    WaitWhileWaiter(state);
    old_value = state->load(std::memory_order_acquire);
  }
}

// Release and abort publish the construction's stores (release) and wake every
// waiter only when one announced itself.
void GuardSet(void* guard, int32_t value) {
  auto* state = static_cast<GuardState*>(guard);
  int32_t old_value = state->exchange(value, std::memory_order_release);
  if (old_value == kUnderwayWithWaiter) WakeByAddressAll(reinterpret_cast<void*>(state));
}
void BIONIC_SYSV GuardRelease(void* guard) { GuardSet(guard, kComplete); }
void BIONIC_SYSV GuardAbort(void* guard) { GuardSet(guard, kNotYetStarted); }

// --- __cxa_pure_virtual.cpp -----------------------------------------------------------

[[noreturn]] void BIONIC_SYSV PureVirtual() {
  Fatal("Pure virtual function called. Are you calling virtual methods from a destructor?");
}

}  // namespace

void RegisterCxx(SymbolTable& t) {
#define E(n, f) t[n] = reinterpret_cast<void*>(&f)
  t["_ZSt7nothrow"] = const_cast<unsigned char*>(&g_nothrow);
  E("_Znwm", New);
  E("_Znam", NewArray);
  E("_ZnwmRKSt9nothrow_t", NewNothrow);
  E("_ZnamRKSt9nothrow_t", NewNothrow);
  E("_ZdlPv", Delete);
  E("_ZdaPv", Delete);
  E("_ZdlPvRKSt9nothrow_t", DeleteNothrow);
  E("_ZdaPvRKSt9nothrow_t", DeleteNothrow);
  E("__cxa_guard_acquire", GuardAcquire);
  E("__cxa_guard_release", GuardRelease);
  E("__cxa_guard_abort", GuardAbort);
  E("__cxa_pure_virtual", PureVirtual);
#undef E
}

}  // namespace gesher::bionic
