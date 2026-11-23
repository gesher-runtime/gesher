// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unwind.h>
#include "cleanup_probe.h"

class DllResource {
 public:
  explicit DllResource(CleanupLink* link) : link_(link) {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    memory_ = HeapAlloc(GetProcessHeap(), 0, 96);
    link_->notify(link_->state, CLEANUP_ENTER, link_->level * 100 + 20,
                  event_ != nullptr && memory_ != nullptr);
  }
  ~DllResource() noexcept {
    const bool closed = CloseHandle(event_) != 0;
    const bool freed = HeapFree(GetProcessHeap(), 0, memory_) != 0;
    link_->notify(link_->state, CLEANUP_LEAVE, link_->level * 100 + 20,
                  closed && freed);
  }
  DllResource(const DllResource&) = delete;
  DllResource& operator=(const DllResource&) = delete;
 private:
  CleanupLink* link_;
  HANDLE event_;
  void* memory_;
};

// This DLL, like gesher_arm64.dll, links its own pinned static libunwind.
// Forced cleanup must resume through this copy and still reach the EXE owner.
extern "C" __declspec(dllexport) __declspec(noinline)
uint64_t CleanupDllCall(CleanupLink* link) {
  DllResource resource(link);
  const uint64_t value = CleanupCBridge(link);
  link->notify(link->state, CLEANUP_AFTER, link->level * 100 + 21, 1);
  return value;
}

extern "C" __declspec(dllexport) void* CleanupDllUnwinder() {
  return reinterpret_cast<void*>(&_Unwind_ForcedUnwind);
}
