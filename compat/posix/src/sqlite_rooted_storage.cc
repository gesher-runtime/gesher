// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

// Keep SQLite's native Win32 VFS and its real locking, mapping and pager. Only
// the path-based file operations need a broker when the original filename is
// in a contained standalone application's admitted data roots.
#include <errno.h>
#include <sqlite3.h>
#include <windows.h>

#include "rooted_storage_client.h"

namespace {
namespace storage = gesher::storage_root;
namespace wire = gesher::rooted_storage_wire;

void SetStorageError(int error) {
  DWORD native = ERROR_IO_DEVICE;
  switch (error) {
    case EACCES: case EPERM: native = ERROR_ACCESS_DENIED; break;
    case ENOENT: native = ERROR_FILE_NOT_FOUND; break;
    case ENOTDIR: native = ERROR_DIRECTORY; break;
    case EISDIR: native = ERROR_ACCESS_DENIED; break;
    case EEXIST: native = ERROR_FILE_EXISTS; break;
    case ENAMETOOLONG: native = ERROR_FILENAME_EXCED_RANGE; break;
    case ENOSPC: native = ERROR_DISK_FULL; break;
    case EROFS: native = ERROR_WRITE_PROTECT; break;
    case EBUSY: case EAGAIN: native = ERROR_SHARING_VIOLATION; break;
    case EINVAL: native = ERROR_INVALID_PARAMETER; break;
    case EOPNOTSUPP: native = ERROR_NOT_SUPPORTED; break;
    case ELOOP: native = ERROR_CANT_ACCESS_FILE; break;
    case ENOMEM: native = ERROR_NOT_ENOUGH_MEMORY; break;
    case ETIMEDOUT: native = ERROR_TIMEOUT; break;
  }
  SetLastError(native);
}

bool ReadInfo(const storage::Selection& path, BY_HANDLE_FILE_INFORMATION* info) {
  HANDLE h;
  const int error = storage::Open(path, wire::kMetadata, &h);
  if (error != 0) {
    SetStorageError(error);
    return false;
  }
  const bool okay = GetFileInformationByHandle(h, info);
  const DWORD last_error = GetLastError();
  CloseHandle(h);
  SetLastError(last_error);
  return okay;
}
}  // namespace

extern "C" int gesher_sqlite_rooted_access_w(LPCWSTR name, int access) {
  if (access != SQLITE_ACCESS_READWRITE) return 1;
  const auto path = storage::Lookup(name);
  if (path.error != 0) return 0;
  if (!path.IsInstalledMedia()) return 1;
  // xAccess reads filesystem attributes through the broker afterward. Those
  // attributes cannot express an app's current read-only Android grant.
  // Match POSIX access(W_OK): its metadata-only request works for directories
  // and files, checks the live grant, and never creates or changes an object.
  HANDLE handle;
  constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  const uint32_t flags = wire::kMetadataWrite | wire::kShareOverride | (share << wire::kShareShift);
  if (storage::Open(path, flags, &handle) != 0) return 0;
  CloseHandle(handle);
  return 1;
}

extern "C" HANDLE WINAPI gesher_sqlite_create_file_w(LPCWSTR name, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD attributes, HANDLE template_file) {
  const auto path = name == nullptr ? storage::Selection{} : storage::Lookup(name);
  if (!path) return CreateFileW(name, access, share, security, disposition, attributes, template_file);
  if (path.error != 0) {
    SetStorageError(path.error);
    return INVALID_HANDLE_VALUE;
  }
  constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  constexpr DWORD kAttributes = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY | FILE_FLAG_DELETE_ON_CLOSE |
      FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH;
  static_assert(kShare == 7);
  if (security != nullptr || template_file != nullptr || (share & ~kShare) != 0 ||
      (attributes & ~kAttributes) != 0 || (access & ~(GENERIC_READ | GENERIC_WRITE)) != 0) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
  }
  uint32_t flags;
  switch (access) {
    case GENERIC_READ: flags = wire::kRead; break;
    case GENERIC_WRITE: flags = wire::kWrite; break;
    case GENERIC_READ | GENERIC_WRITE: flags = wire::kReadWrite; break;
    default: flags = wire::kMetadata; break;
  }
  switch (disposition) {
    case OPEN_EXISTING: break;
    case CREATE_NEW: flags |= wire::kCreate | wire::kExclusive; break;
    case OPEN_ALWAYS: flags |= wire::kCreate; break;
    case CREATE_ALWAYS: flags |= wire::kCreate | wire::kTruncate; break;
    case TRUNCATE_EXISTING: flags |= wire::kTruncate; break;
    default: SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
  }
  flags |= wire::kShareOverride | (share << wire::kShareShift);
  if ((flags & wire::kCreate) != 0 && (attributes & FILE_ATTRIBUTE_READONLY) != 0) flags |= wire::kReadOnlyCreate;
  if ((attributes & FILE_FLAG_WRITE_THROUGH) != 0) flags |= wire::kWriteThrough;
  if ((attributes & FILE_FLAG_DELETE_ON_CLOSE) != 0) flags |= wire::kDeleteOnClose;
  if ((attributes & FILE_FLAG_OVERLAPPED) != 0) flags |= wire::kOverlapped;
  HANDLE h;
  bool created = false;
  const int error = storage::Open(path, flags, &h, &created);
  if (error != 0) {
    SetStorageError(error);
    return INVALID_HANDLE_VALUE;
  }
  SetLastError((disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) && !created
                   ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
  return h;
}

extern "C" BOOL WINAPI gesher_sqlite_delete_file_w(LPCWSTR name) {
  const auto path = name == nullptr ? storage::Selection{} : storage::Lookup(name);
  if (!path) return DeleteFileW(name);
  const int error = storage::Remove(path, false);
  if (error != 0) SetStorageError(error);
  return error == 0;
}

extern "C" DWORD WINAPI gesher_sqlite_get_file_attributes_w(LPCWSTR name) {
  const auto path = name == nullptr ? storage::Selection{} : storage::Lookup(name);
  if (!path) return GetFileAttributesW(name);
  BY_HANDLE_FILE_INFORMATION info;
  return ReadInfo(path, &info) ? info.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
}

extern "C" BOOL WINAPI gesher_sqlite_get_file_attributes_ex_w(LPCWSTR name,
    GET_FILEEX_INFO_LEVELS level, LPVOID result) {
  const auto path = name == nullptr ? storage::Selection{} : storage::Lookup(name);
  if (!path) return GetFileAttributesExW(name, level, result);
  if (level != GetFileExInfoStandard || result == nullptr) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!ReadInfo(path, &info)) return FALSE;
  *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(result) = {info.dwFileAttributes, info.ftCreationTime,
      info.ftLastAccessTime, info.ftLastWriteTime, info.nFileSizeHigh, info.nFileSizeLow};
  return TRUE;
}
