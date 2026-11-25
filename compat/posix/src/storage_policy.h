// gesher-base: external storage path access policy (scoped storage &
// MANAGE_EXTERNAL_STORAGE enforcement).
#pragma once

#include <string>

namespace gesher {

enum class AccessKind {
  kStat,   // existence / metadata check (stat, lstat, fstatat, access(F_OK), realpath)
  kRead,   // read / list / readlink (O_RDONLY, opendir, readlink, access(R_OK|X_OK), chdir)
  kWrite,  // create / modify / delete (O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND, mkdir, rmdir, unlink, rename, chmod, link, symlink, utimensat, access(W_OK))
};

// Checks whether the current process (identified by $GESHER_APP_PACKAGE) is
// allowed to perform `kind` on `windows_path`.
// Returns true if allowed; returns false and sets errno = EACCES if denied.
bool CheckExternalStorageAccess(const std::wstring& windows_path, AccessKind kind);

}  // namespace gesher
