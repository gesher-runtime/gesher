// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Explicit host boundary: only Windows thread creation, join and handle lifetime.
// QueryPool's synchronization, admission, cancellation and deadline are unmodified.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
using DWORD = uint32_t;
using LPTHREAD_START_ROUTINE = DWORD (*)(void*);
#define WINAPI
constexpr DWORD WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffffU;
constexpr DWORD INFINITE = 0xffffffffU;
struct QaThread {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::thread native;
};
using HANDLE = QaThread*;
inline std::atomic<unsigned> qa_handles{0};
inline HANDLE CreateThread(void*, size_t, LPTHREAD_START_ROUTINE entry, void* value,
                           DWORD, DWORD*) {
  auto* handle = new QaThread;
  try {
    handle->native = std::thread([=] {
      entry(value);
      { std::lock_guard<std::mutex> lock(handle->mutex); handle->done = true; }
      handle->changed.notify_all();
    });
  } catch (...) { delete handle; return nullptr; }
  ++qa_handles;
  return handle;
}
inline DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
  if (!handle) return WAIT_FAILED;
  std::unique_lock<std::mutex> lock(handle->mutex);
  if (milliseconds == INFINITE) handle->changed.wait(lock, [&] { return handle->done; });
  else if (!handle->changed.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                    [&] { return handle->done; })) return WAIT_TIMEOUT;
  return WAIT_OBJECT_0;
}
inline bool CloseHandle(HANDLE handle) {
  if (!handle) return false;
  handle->native.join();
  delete handle;
  --qa_handles;
  return true;
}
