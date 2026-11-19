// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <cerrno>
#include <cstdio>

namespace gesher::bionic::stdio_windows {

// Gesher uses the 64-bit UCRT. In its private FILE layout, the recursive
// critical section follows two pointers, five 32-bit fields and another
// pointer (after padding). Validate that layout using a private stream before
// operating on an application stream. There is no public UCRT try-lock API.
constexpr size_t kStreamLockOffset = 48;
static_assert(sizeof(void*) == 8 && kStreamLockOffset % alignof(CRITICAL_SECTION) == 0);

inline CRITICAL_SECTION* StreamLockAddress(FILE* stream) {
  return reinterpret_cast<CRITICAL_SECTION*>(
      reinterpret_cast<unsigned char*>(stream) + kStreamLockOffset);
}

inline bool ReadLockState(FILE* stream, LONG depth, DWORD owner) {
  CRITICAL_SECTION state{};
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), StreamLockAddress(stream), &state,
                           sizeof(state), &read) &&
      read == sizeof(state) && state.RecursionCount == depth &&
      reinterpret_cast<ULONG_PTR>(state.OwningThread) == owner;
}

inline bool VerifyStreamLockLayout() {
  int saved_errno = errno;
  DWORD saved_error = GetLastError();
  FILE* probe = _wfopen(L"NUL", L"rb");
  bool valid = false;
  if (probe) {
    DWORD owner = GetCurrentThreadId();
    bool initially_unlocked = ReadLockState(probe, 0, 0);
    _lock_file(probe);
    bool first_lock = ReadLockState(probe, 1, owner);
    _lock_file(probe);
    bool recursive_lock = ReadLockState(probe, 2, owner);
    _unlock_file(probe);
    bool first_unlock = ReadLockState(probe, 1, owner);
    _unlock_file(probe);
    valid = initially_unlocked && first_lock && recursive_lock && first_unlock &&
        ReadLockState(probe, 0, 0);
    std::fclose(probe);
  }
  SetLastError(saved_error);
  errno = saved_errno;
  return valid;
}

inline bool StreamLockLayoutSupported() {
  static const bool supported = VerifyStreamLockLayout();
  return supported;
}

enum class TryLockResult { kAcquired, kBusy, kUnsupported };

inline TryLockResult TryLock(FILE* stream) {
  if (!StreamLockLayoutSupported()) return TryLockResult::kUnsupported;
  return TryEnterCriticalSection(StreamLockAddress(stream)) ?
      TryLockResult::kAcquired : TryLockResult::kBusy;
}

}  // namespace gesher::bionic::stdio_windows
