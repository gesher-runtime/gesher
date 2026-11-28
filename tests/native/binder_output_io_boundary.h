// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Host-only I/O boundary: actual local byte streams and self-process FDs.
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

using HANDLE = void*;
using DWORD = unsigned long;
constexpr int FALSE = 0;
constexpr DWORD PROCESS_DUP_HANDLE = 1, DUPLICATE_SAME_ACCESS = 2, DUPLICATE_CLOSE_SOURCE = 4;
inline HANDLE HostHandle(int fd) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd)); }
inline int HostFd(HANDLE handle) { return static_cast<int>(reinterpret_cast<uintptr_t>(handle)); }
inline HANDLE HostProcess() { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(-2)); }
inline DWORD GetLastError() { return static_cast<DWORD>(errno); }
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
inline HANDLE OpenProcess(DWORD, int, DWORD pid) {
  if (pid == GetCurrentProcessId()) return HostProcess();
  errno = ESRCH;
  return nullptr;
}
inline bool CloseHandle(HANDLE handle) { return handle == HostProcess() || close(HostFd(handle)) == 0; }
inline bool DuplicateHandle(HANDLE from, HANDLE source, HANDLE to, HANDLE* target, DWORD, int, DWORD flags) {
  if (from != HostProcess() || (to != nullptr && to != HostProcess())) { errno = ESRCH; return false; }
  if (flags & DUPLICATE_CLOSE_SOURCE) return close(HostFd(source)) == 0;
  if (to == nullptr || target == nullptr) { errno = EINVAL; return false; }
  int copied = dup(HostFd(source));
  if (copied < 0) return false;
  *target = HostHandle(copied);
  return true;
}
namespace pipe_fault {
inline std::atomic<int> descriptor{-1};
inline std::atomic<size_t> max_chunk{std::numeric_limits<size_t>::max()};
inline std::atomic<size_t> remaining{std::numeric_limits<size_t>::max()};
}
inline bool ReadFile(HANDLE handle, void* output, DWORD wanted, DWORD* got, void*) {
  ssize_t count;
  do { count = read(HostFd(handle), output, wanted); } while (count < 0 && errno == EINTR);
  *got = count > 0 ? static_cast<DWORD>(count) : 0;
  return count >= 0;
}
inline bool WriteFile(HANDLE handle, const void* input, DWORD wanted, DWORD* wrote, void*) {
  size_t amount = wanted;
  const bool fault = HostFd(handle) == pipe_fault::descriptor.load();
  if (fault) {
    amount = std::min(amount, pipe_fault::max_chunk.load());
    amount = std::min(amount, pipe_fault::remaining.load());
    if (amount == 0) { *wrote = 0; errno = EIO; return false; }
  }
  ssize_t count;
  do { count = send(HostFd(handle), input, amount, MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
  *wrote = count > 0 ? static_cast<DWORD>(count) : 0;
  if (fault && count > 0 && pipe_fault::remaining.load() != std::numeric_limits<size_t>::max())
    pipe_fault::remaining.fetch_sub(static_cast<size_t>(count));
  return count >= 0;
}
