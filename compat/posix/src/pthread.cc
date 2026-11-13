// gesher-base: <pthread.h> additions.

#include <pthread.h>

#include <errno.h>
#include <locale.h>
#include <time.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "internal.h"

// The real winpthreads calls; <pthread.h> routes everyone else here.
#undef pthread_key_create
#undef pthread_key_delete
#undef pthread_setname_np
#undef pthread_getname_np

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

thread_local bool self_named = false;
constexpr size_t kTaskCommLen = 16;  // Linux's TASK_COMM_LEN, with the NUL

// The first |limit| bytes of UTF-8 |text| at most, not splitting a character.
size_t Utf8Prefix(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text.size();
  size_t end = limit;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
  return end;
}

// Fallback for an unnamed detached thread, whose name winpthreads cannot
// read. This is the executable's basename; thread creation/inherited Linux
// comm names are outside this adapter.
std::string ProcessName() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring name(path, n < MAX_PATH ? n : 0);
  size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos) name = name.substr(slash + 1);
  size_t dot = name.rfind(L'.');
  if (dot != std::wstring::npos) name.resize(dot);
  return gesher::Narrow(name.c_str());
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

int gesher_pthread_setname_np(pthread_t thread, const char* name) {
  if (name == nullptr) return EINVAL;
  if (std::strlen(name) >= kTaskCommLen) return ERANGE;
  // Keep cross-thread ownership with winpthreads; never borrow a native handle
  // that join/detach can close. Its mbsrtowcs conversion uses the caller's CRT
  // locale, so scope UTF-8 to this thread and restore both locale and mode.
  if (!pthread_equal(thread, pthread_self())) {
    int previous_mode = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    if (previous_mode == -1) return EINVAL;
    // Arm restoration before copying the locale string: that allocation can
    // fail in an exception-enabled build. LC_CTYPE is still
    // unchanged then, so restoring only the mode closes that exceptional path.
    struct LocaleMode {
      int previous;
      ~LocaleMode() { if (previous != -1) _configthreadlocale(previous); }
      bool Restore() {
        int value = previous;
        previous = -1;
        return _configthreadlocale(value) != -1;
      }
    } mode{previous_mode};
    const char* current = setlocale(LC_CTYPE, nullptr);
    std::string saved = current ? current : "";
    if (!current || !setlocale(LC_CTYPE, ".UTF8")) {
      mode.Restore();
      return EINVAL;
    }
    int result = pthread_setname_np(thread, name);
    bool restored = setlocale(LC_CTYPE, saved.c_str()) != nullptr;
    restored = mode.Restore() && restored;
    return result != 0 ? result : restored ? 0 : EINVAL;
  }
  int cached = pthread_setname_np(thread, name);  // ESRCH is expected for detached threads
  if (cached != 0 && cached != ESRCH) return cached;
  std::wstring wide = gesher::Widen(name);
  if (FAILED(SetThreadDescription(GetCurrentThread(), wide.c_str()))) return ESRCH;
  self_named = true;
  return 0;
}

int gesher_pthread_getname_np(pthread_t thread, char* name, size_t len) {
  if (name == nullptr || len < kTaskCommLen) return ERANGE;
  if (!pthread_equal(thread, pthread_self())) return pthread_getname_np(thread, name, len);
  PWSTR description = nullptr;
  if (FAILED(GetThreadDescription(GetCurrentThread(), &description))) return ESRCH;
  std::string text = description != nullptr ? gesher::Narrow(description) : std::string();
  if (description != nullptr) LocalFree(description);
  if (text.empty() && !self_named) {
    // An explicitly empty name is valid. For a joinable thread, winpthreads'
    // successful empty read is authoritative too (including a peer's rename).
    char cached[kTaskCommLen]{};
    if (pthread_getname_np(thread, cached, sizeof(cached)) != 0) text = ProcessName();
  }
  text.resize(Utf8Prefix(text, kTaskCommLen - 1));
  std::memcpy(name, text.c_str(), text.size() + 1);
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
