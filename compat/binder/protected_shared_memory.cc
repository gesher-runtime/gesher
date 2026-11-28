// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "protected_shared_memory.h"

#include <memory>

namespace gesher::binder {
namespace {

constexpr DWORD kReadOnlyAccess = FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr uint64_t kMaximumRegionSize = 1024 * 1024;

struct CloseHandleOwner {
  void operator()(void* value) const { CloseHandle(value); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;

bool ValidHandle(uint64_t value) {
  return value != 0 && value <= static_cast<uint64_t>(INTPTR_MAX);
}

bool SameObject(HANDLE left, HANDLE right) {
  using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
  static const auto compare = reinterpret_cast<Compare>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles")));
  // The protected path requires exact-object comparison; do not weaken this to
  // a pathname, file identifier or equal bytes on an older Windows host.
  return compare && compare(left, right);
}

DWORD ReadOnlySource(const ProtectedPeers::Peer& producer, uint64_t value, HANDLE* result) {
  if (!producer || !producer->live() || producer->role() != PeerRole::kSystemServer ||
      producer->uid() != 1000) return ERROR_ACCESS_DENIED;
  if (!ValidHandle(value)) return ERROR_INVALID_HANDLE;
  if (!DuplicateHandle(producer->process(), reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)),
                       GetCurrentProcess(), result, kReadOnlyAccess, FALSE, 0)) return GetLastError();
  return ERROR_SUCCESS;
}

DWORD CheckFile(HANDLE file, uint64_t size) {
  FILE_STANDARD_INFO info{};
  if (GetFileType(file) != FILE_TYPE_DISK ||
      !GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)) ||
      info.Directory || info.EndOfFile.QuadPart < 0 ||
      static_cast<uint64_t>(info.EndOfFile.QuadPart) != size) return ERROR_INVALID_DATA;
  return ERROR_SUCCESS;
}

}  // namespace

ProtectedSharedMemory::~ProtectedSharedMemory() {
  if (object_) CloseHandle(object_);
}

DWORD ProtectedSharedMemory::Register(const ProtectedPeers::Peer& producer,
                                      uint64_t source_handle, uint64_t size) {
  if (size == 0 || size > kMaximumRegionSize) return ERROR_INVALID_PARAMETER;
  HANDLE raw = nullptr;
  DWORD status = ReadOnlySource(producer, source_handle, &raw);
  if (status != ERROR_SUCCESS) return status;
  const Handle candidate(raw);
  status = CheckFile(candidate.get(), size);
  if (status != ERROR_SUCCESS) return status;
  std::lock_guard lock(mutex_);
  if (!producer->live()) return ERROR_PROCESS_ABORTED;
  if (object_) {
    return producer == producer_ && size == size_ && SameObject(object_, candidate.get())
        ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
  }
  // Prove that this host implements the comparison before accepting a region.
  if (!SameObject(candidate.get(), candidate.get())) return ERROR_NOT_SUPPORTED;
  if (!DuplicateHandle(GetCurrentProcess(), candidate.get(), GetCurrentProcess(), &object_,
                       kReadOnlyAccess, FALSE, 0)) return GetLastError();
  producer_ = producer;
  size_ = size;
  return ERROR_SUCCESS;
}

DWORD ProtectedSharedMemory::Duplicate(const ProtectedPeers::Peer& producer,
                                       const ProtectedPeers::Peer& consumer,
                                       uint64_t source_handle, uint64_t* remote_handle) {
  if (!remote_handle || *remote_handle) return ERROR_INVALID_PARAMETER;
  if (!consumer || !consumer->live() || consumer->role() != PeerRole::kClient ||
      consumer->uid() < 10000) return ERROR_ACCESS_DENIED;
  HANDLE raw = nullptr;
  DWORD status = ReadOnlySource(producer, source_handle, &raw);
  if (status != ERROR_SUCCESS) return status;
  const Handle candidate(raw);
  std::lock_guard lock(mutex_);
  if (!object_ || producer != producer_ || !producer->live() || !consumer->live() ||
      !SameObject(object_, candidate.get())) return ERROR_ACCESS_DENIED;
  status = CheckFile(candidate.get(), size_);
  if (status != ERROR_SUCCESS) return status;
  HANDLE remote = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), object_, consumer->process(), &remote,
                       kReadOnlyAccess, FALSE, 0)) return GetLastError();
  if (!producer->live() || !consumer->live()) {
    DuplicateHandle(consumer->process(), remote, nullptr, nullptr, 0, FALSE,
                    DUPLICATE_CLOSE_SOURCE);
    return ERROR_PROCESS_ABORTED;
  }
  *remote_handle = reinterpret_cast<uintptr_t>(remote);
  return ERROR_SUCCESS;
}

}  // namespace gesher::binder
