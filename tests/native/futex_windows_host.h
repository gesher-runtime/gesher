// Host model of the Win32 wait transport. The production queue/selection,
// deadlines, ownership and guest argument conversion are compiled unchanged.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#define __declspec(x)
using DWORD = uint32_t;
using HMODULE = void*;
using FARPROC = void (*)();
using SRWLOCK = std::mutex;
#define SRWLOCK_INIT {}
static constexpr DWORD INFINITE = 0xffffffff, ERROR_TIMEOUT = 1460;
static thread_local DWORD last_windows_error;
static void AcquireSRWLockExclusive(SRWLOCK* p) { p->lock(); }
static void ReleaseSRWLockExclusive(SRWLOCK* p) { p->unlock(); }
static DWORD GetLastError() { return last_windows_error; }
static DWORD GetCurrentThreadId() { return 1; }
static DWORD GetCurrentProcessId() { return 1; }
static HMODULE GetModuleHandleW(const wchar_t*) { return reinterpret_cast<void*>(1); }
static FARPROC GetProcAddress(HMODULE, const char*);
struct AddressWait { std::condition_variable cv; unsigned users = 0; };
static std::mutex address_wait_mutex;
static std::unordered_map<void*, std::shared_ptr<AddressWait>> address_waits;
static std::atomic<int> injected_spurious{0};
static bool WaitOnAddress(volatile void* raw, void* expected, size_t bytes, DWORD ms) {
  if (injected_spurious.exchange(0)) return true;
  auto* address = const_cast<void*>(raw);
  if (bytes != sizeof(int)) return false;
  int value; std::memcpy(&value, expected, sizeof(value));
  std::unique_lock<std::mutex> lock(address_wait_mutex);
  auto& slot = address_waits[address]; if (!slot) slot = std::make_shared<AddressWait>();
  auto wait = slot; ++wait->users;
  auto changed = [&] { return __atomic_load_n(static_cast<int*>(address), __ATOMIC_ACQUIRE) != value; };
  bool ok = true;
  if (ms == INFINITE) wait->cv.wait(lock, changed);
  else ok = wait->cv.wait_for(lock, std::chrono::milliseconds(ms), changed);
  if (--wait->users == 0) address_waits.erase(address);
  if (!ok) last_windows_error = ERROR_TIMEOUT;
  return ok;
}
static void WakeByAddressSingle(void* address) {
  std::lock_guard<std::mutex> lock(address_wait_mutex);
  auto it = address_waits.find(address); if (it != address_waits.end()) it->second->cv.notify_one();
}
static void WakeByAddressAll(void* address) {
  std::lock_guard<std::mutex> lock(address_wait_mutex);
  auto it = address_waits.find(address); if (it != address_waits.end()) it->second->cv.notify_all();
}
