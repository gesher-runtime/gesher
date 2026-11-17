// SQLite's Win32 VFS canonicalizes names before the pager allocates its
// filename/URI/journal metadata. Translate at that boundary, using the same
// namespace as Java/POSIX file operations. Compiled into the sqlite target.
#include "internal.h"

#include <sqlite3.h>

extern "C" int gesher_sqlite_full_pathname(
    sqlite3_vfs* vfs, const char* path, int capacity, char* full,
    int (*native_full_pathname)(sqlite3_vfs*, const char*, int, char*)) {
  if (capacity <= 0 || full == nullptr) return SQLITE_CANTOPEN;
  // SQLite examines the output even when xFullPathname fails.
  full[0] = '\0';
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EINVAL)) return SQLITE_CANTOPEN;
  const std::string native = gesher::Narrow(windows.c_str());
  if (native.empty()) return SQLITE_CANTOPEN;
  // Keep Win32's canonicalization and its exact error codes. In particular,
  // do not copy or replace xOpen's sqlite3_filename: its pointer and hidden
  // URI/journal metadata belong to SQLite and survive until xClose.
  return native_full_pathname(vfs, native.c_str(), capacity, full);
}
