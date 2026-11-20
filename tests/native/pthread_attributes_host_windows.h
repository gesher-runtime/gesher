// Linux boundary for exact production thread-registry code. This supplies
// retained kernel-object-like handles and real host pthreads/stack intervals;
// it does not claim Win32 execution. The independent Windows fixture supplies
// that integration gate without this header.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

using DWORD = uint32_t;
using ULONG_PTR = uintptr_t;
using SIZE_T = size_t;
using BOOL = int;
#define WINAPI
constexpr BOOL FALSE = 0;
constexpr DWORD INFINITE = UINT32_MAX;
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_TIMEOUT = 258;
constexpr DWORD WAIT_FAILED = UINT32_MAX;
constexpr DWORD THREAD_QUERY_LIMITED_INFORMATION = 0x800;
constexpr DWORD THREAD_SET_LIMITED_INFORMATION = 0x400;
constexpr DWORD SYNCHRONIZE = 0x100000;
constexpr DWORD STACK_SIZE_PARAM_IS_A_RESERVATION = 0x10000;
constexpr DWORD MEM_COMMIT = 0x1000;
constexpr DWORD MEM_RESERVE = 0x2000;
constexpr DWORD PAGE_READWRITE = 0x04;

// Committed, readable/writable anonymous pages; production never frees them.
inline void* VirtualAlloc(void* address, SIZE_T size, DWORD type, DWORD protect) {
  if (type != (MEM_COMMIT | MEM_RESERVE) || protect != PAGE_READWRITE) std::abort();
  void* pages = mmap(address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return pages == MAP_FAILED ? nullptr : pages;
}

namespace test_windows {
struct Thread {
  std::mutex mutex;
  std::condition_variable changed;
  bool finished = false;
};
struct Handle { std::shared_ptr<Thread> thread; };
inline std::atomic<int> handles{0};
inline std::atomic<bool> hold_start{false};
inline std::atomic<bool> fail_create{false};
inline std::atomic<bool> fail_duplicate{false};
struct Current {
  std::shared_ptr<Thread> thread;
  ~Current() {
    if (!thread) return;
    {
      std::lock_guard<std::mutex> lock(thread->mutex);
      thread->finished = true;
    }
    thread->changed.notify_all();
  }
};
inline thread_local Current current;
inline std::shared_ptr<Thread> Self() {
  if (!current.thread) current.thread = std::make_shared<Thread>();
  return current.thread;
}
}
using HANDLE = test_windows::Handle*;
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(intptr_t{-2}); }
inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(intptr_t{-1}); }
inline DWORD GetCurrentThreadId() { return static_cast<DWORD>(syscall(SYS_gettid)); }
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
inline void GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high) {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) std::abort();
  void* base = nullptr;
  size_t size = 0;
  if (pthread_attr_getstack(&attr, &base, &size) != 0) std::abort();
  pthread_attr_destroy(&attr);
  *low = reinterpret_cast<ULONG_PTR>(base);
  *high = *low + size;
}
inline std::shared_ptr<test_windows::Thread> ThreadFromHandle(HANDLE handle) {
  return handle == GetCurrentThread() ? test_windows::Self() : handle->thread;
}
inline BOOL DuplicateHandle(HANDLE, HANDLE source, HANDLE, HANDLE* out, DWORD, BOOL, DWORD) {
  if (test_windows::fail_duplicate.load()) return FALSE;
  *out = new test_windows::Handle{ThreadFromHandle(source)};
  ++test_windows::handles;
  return 1;
}
inline BOOL CloseHandle(HANDLE handle) {
  delete handle;
  --test_windows::handles;
  return 1;
}
inline DWORD WaitForSingleObject(HANDLE handle, DWORD timeout) {
  if (!handle) return WAIT_FAILED;
  auto target = ThreadFromHandle(handle);
  std::unique_lock<std::mutex> lock(target->mutex);
  if (timeout == INFINITE) target->changed.wait(lock, [&] { return target->finished; });
  else if (!target->changed.wait_for(lock, std::chrono::milliseconds(timeout),
                                      [&] { return target->finished; })) return WAIT_TIMEOUT;
  return WAIT_OBJECT_0;
}
inline HANDLE CreateThread(void*, SIZE_T stack, DWORD (*start)(void*), void* argument,
                            DWORD, DWORD* tid) {
  if (test_windows::fail_create.load()) return nullptr;
  struct Start {
    std::shared_ptr<test_windows::Thread> thread;
    DWORD (*routine)(void*);
    void* argument;
  };
  auto state = std::make_shared<test_windows::Thread>();
  auto* input = new Start{state, start, argument};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (stack) pthread_attr_setstacksize(&attr, stack);
  pthread_t native{};
  int result = pthread_create(&native, &attr, [](void* raw) -> void* {
    std::unique_ptr<Start> input(static_cast<Start*>(raw));
    test_windows::current.thread = input->thread;
    while (test_windows::hold_start.load()) std::this_thread::yield();
    input->routine(input->argument);
    return nullptr;
  }, input);
  pthread_attr_destroy(&attr);
  if (result) { delete input; return nullptr; }
  pthread_detach(native);
  if (tid) *tid = 1;  // TID naming/clock operations are outside this fixture.
  ++test_windows::handles;
  return new test_windows::Handle{std::move(state)};
}
[[noreturn]] inline void ExitThread(DWORD) { pthread_exit(nullptr); }
inline BOOL SwitchToThread() { sched_yield(); return 1; }
inline BOOL WaitOnAddress(volatile void* address, void* expected, SIZE_T size, DWORD timeout) {
  if (size != sizeof(int32_t)) std::abort();
  auto* word = reinterpret_cast<std::atomic<int32_t>*>(const_cast<void*>(address));
  int32_t before;
  std::memcpy(&before, expected, sizeof before);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  while (word->load(std::memory_order_acquire) == before) {
    if (timeout != INFINITE && std::chrono::steady_clock::now() >= deadline) return FALSE;
    std::this_thread::yield();
  }
  return 1;
}
inline void WakeByAddressAll(void*) {}
