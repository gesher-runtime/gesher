// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <gesher/filesystem_capacity.h>

#include <winternl.h>

#include "internal.h"

namespace gesher {
namespace {

// FILE_FS_FULL_SIZE_INFORMATION in the Windows Driver Kit.
struct VolumeCapacity {
  LARGE_INTEGER total_units;
  LARGE_INTEGER caller_free_units;
  LARGE_INTEGER total_free_units;
  ULONG sectors_per_unit;
  ULONG bytes_per_sector;
};
static_assert(sizeof(VolumeCapacity) == 32 && alignof(VolumeCapacity) >= 8);

bool CapacityBytes(LONGLONG units, uint64_t unit, uint64_t* result) {
  if (units < 0 || static_cast<uint64_t>(units) > UINT64_MAX / unit) return false;
  *result = static_cast<uint64_t>(units) * unit;
  return true;
}

}  // namespace

int QueryFilesystemCapacity(HANDLE file, rooted_storage_wire::Filesystem* capacity) {
  if (capacity == nullptr) return EFAULT;
  if (file == nullptr || file == INVALID_HANDLE_VALUE || GetFileType(file) != FILE_TYPE_DISK) return EBADF;
  using Query = NTSTATUS(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FS_INFORMATION_CLASS);
  using Translate = ULONG(WINAPI*)(NTSTATUS);
  static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  static const auto query = reinterpret_cast<Query>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryVolumeInformationFile")));
  static const auto translate = reinterpret_cast<Translate>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (query == nullptr || translate == nullptr) return EOPNOTSUPP;
  constexpr NTSTATUS kPending = 0x00000103;
  VolumeCapacity info{};
  IO_STATUS_BLOCK io{};
  io.Status = kPending;
  NTSTATUS status = query(file, &io, &info, sizeof(info), FileFsFullSizeInformation);
  if (status == kPending) {
    // An overlapped file can complete a different operation first. Keep this
    // operation's buffer alive until its own I/O status changes.
    auto* completion = reinterpret_cast<volatile LONG*>(&io.Status);
    do {
      Sleep(1);
      status = InterlockedCompareExchange(completion, 0, 0);
    } while (status == kPending);
  }
  if (status < 0) return ErrnoFromWin32(translate(status));
  if (io.Information < sizeof(info)) return EIO;
  const uint64_t unit = static_cast<uint64_t>(info.sectors_per_unit) * info.bytes_per_sector;
  rooted_storage_wire::Filesystem result{};
  if (unit == 0 || !CapacityBytes(info.caller_free_units.QuadPart, unit, &result.available_bytes) ||
      !CapacityBytes(info.total_units.QuadPart, unit, &result.total_bytes) ||
      !CapacityBytes(info.total_free_units.QuadPart, unit, &result.free_bytes)) return EOVERFLOW;
  *capacity = result;
  return 0;
}

}  // namespace gesher
