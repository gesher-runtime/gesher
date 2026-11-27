// SQLite's Win32 VFS canonicalizes names before the pager allocates its
// filename/URI/journal metadata. Translate at that boundary, using the same
// namespace as Java/POSIX file operations. Compiled into the sqlite target.
#include "internal.h"
#include "storage_policy.h"

#include <sqlite3.h>

// SQLite's VFS receives the canonical name and owns the trailing URI metadata.
// Inspect that name without replacing its pointer. Canonicalization alone has
// no knowledge of the eventual read/write/create/delete request.
extern "C" int gesher_sqlite_path_allowed(const char* path, int access) {
  std::wstring windows;
  // The Win32 VFS syscalls still authorize and perform operations on rooted
  // paths through the broker. This policy preflight only classifies the name.
  if (!gesher::DiskPath(path, &windows, EINVAL, true)) return 0;
  const auto kind = access == SQLITE_ACCESS_READWRITE ? gesher::AccessKind::kWrite
      : access == SQLITE_ACCESS_READ ? gesher::AccessKind::kRead : gesher::AccessKind::kStat;
  return gesher::CheckExternalStorageAccess(windows, kind) ? 1 : 0;
}

extern "C" int gesher_sqlite_open_flags(const char* path, int flags) {
  const bool writes = (flags & (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_DELETEONCLOSE)) != 0;
  if (gesher_sqlite_path_allowed(path, writes ? SQLITE_ACCESS_READWRITE : SQLITE_ACCESS_READ))
    return flags;
  // Preserve SQLite's documented read-only fallback for existing databases.
  // Exclusive creation and delete-on-close cannot become read-only operations.
  if ((flags & SQLITE_OPEN_READWRITE) != 0 &&
      (flags & (SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE)) == 0 &&
      gesher_sqlite_path_allowed(path, SQLITE_ACCESS_READ))
    return (flags & ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) | SQLITE_OPEN_READONLY;
  return 0;
}

extern "C" int gesher_sqlite_handle_allowed(void* handle, int writes) {
  return gesher::CheckExternalStorageHandle(handle, writes ? gesher::AccessKind::kWrite
                                                         : gesher::AccessKind::kRead) ? 1 : 0;
}

extern "C" int gesher_sqlite_full_pathname(
    sqlite3_vfs* vfs, const char* path, int capacity, char* full,
    int (*native_full_pathname)(sqlite3_vfs*, const char*, int, char*)) {
  if (capacity <= 0 || full == nullptr) return SQLITE_CANTOPEN;
  // SQLite examines the output even when xFullPathname fails.
  full[0] = '\0';
  std::wstring windows;
  // The Win32 SQL VFS handles actual selected-root access through its broker
  // wrappers. This step only builds SQLite's owned canonical filename.
  if (!gesher::DiskPath(path, &windows, EINVAL, true)) return SQLITE_CANTOPEN;
  if (!gesher::CheckExternalStorageAccess(windows, gesher::AccessKind::kStat)) return SQLITE_CANTOPEN;
  const std::string native = gesher::Narrow(windows.c_str());
  if (native.empty()) return SQLITE_CANTOPEN;
  // Keep Win32's canonicalization and its exact error codes. In particular,
  // do not copy or replace xOpen's sqlite3_filename: its pointer and hidden
  // URI/journal metadata belong to SQLite and survive until xClose.
  return native_full_pathname(vfs, native.c_str(), capacity, full);
}
