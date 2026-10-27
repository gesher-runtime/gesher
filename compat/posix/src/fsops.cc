// gesher-base: the rest of the calls that take a path (ADR 0045), through the
// path model (path.cc) like open and stat: access, rename, remove, chdir,
// chmod, symbolic and hard links, fopen and freopen, and the *at forms and
// mkdtemp/mkstemp.
//
// access, rename, remove, chdir, chmod, fopen and freopen are the CRT's
// names, defined here as close and read are in fd.cc: gesher-base is linked
// before the CRT, so every caller gets these, with Linux paths (/dev, /proc,
// /mnt/<drive>), UTF-8 names and paths longer than MAX_PATH. (The CRT's take
// the ANSI code page and know none of the Linux names.)
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <windows.h>
#include <bcrypt.h>

#include <string>

#include "internal.h"
#include "record_lock_descriptors.h"

using gesher::PathKind;

namespace {

// The handle's file identity: two names of one file compare equal.
bool SameFile(HANDLE a, const std::wstring& b_path) {
  HANDLE b = CreateFileW(b_path.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (b == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION ia, ib;
  bool same = GetFileInformationByHandle(a, &ia) && GetFileInformationByHandle(b, &ib) &&
              ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
              ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
  CloseHandle(b);
  return same;
}

// FILE_ATTRIBUTE_* of a path without following a final symbolic link, or
// INVALID_FILE_ATTRIBUTES; *link is set for a symbolic link.
DWORD AttributesNoFollow(const std::wstring& path, bool* link) {
  *link = false;
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    return INVALID_FILE_ATTRIBUTES;
  }
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      *link = gesher::IsSymlinkHandle(h);
      CloseHandle(h);
    }
  }
  return data.dwFileAttributes;
}

bool IsEmptyDirectory(const std::wstring& path) {
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &data,
                                 FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return false;
  bool empty = true;
  do {
    if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) empty = false;
  } while (empty && FindNextFileW(find, &data));
  FindClose(find);
  return empty;
}

// The NT form of a full path (\??\C:\..., \??\UNC\...), which
// SetFileInformationByHandle's rename takes.
std::wstring NtPath(const std::wstring& windows) {
  std::wstring full;
  if (windows.rfind(L"\\\\?\\", 0) == 0) {
    full = windows.substr(4);
    if (full.rfind(L"UNC\\", 0) == 0) return L"\\??\\" + full;
  } else {
    DWORD n = GetFullPathNameW(windows.c_str(), 0, nullptr, nullptr);
    if (n == 0) return L"";
    full.resize(n);
    n = GetFullPathNameW(windows.c_str(), n, full.data(), nullptr);
    full.resize(n);
    if (full.rfind(L"\\\\", 0) == 0) return L"\\??\\UNC\\" + full.substr(2);
  }
  return L"\\??\\" + full;
}

constexpr unsigned kRenameNoReplace = 1;  // RENAME_NOREPLACE
[[maybe_unused]] constexpr unsigned kRenameExchange = 2;  // RENAME_EXCHANGE (rejected below)

// rename with Linux's semantics: the new name is replaced atomically, also
// while it is open; a directory replaces an empty directory; renaming a name
// onto another name of the same file does nothing.
int Rename(const char* oldpath, const char* newpath, unsigned flags) {
  if ((flags & ~kRenameNoReplace) != 0) return errno = EINVAL, -1;  // RENAME_EXCHANGE, WHITEOUT
  std::wstring from, to;
  if (!gesher::DiskPath(oldpath, &from, EACCES) || !gesher::DiskPath(newpath, &to, EACCES)) {
    return -1;
  }
  HANDLE h = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) {
    int r = gesher::FailWithLastError();
    CloseHandle(h);
    return r;
  }
  bool from_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !gesher::IsSymlinkHandle(h);
  bool to_link;
  DWORD to_attributes = AttributesNoFollow(to, &to_link);
  bool to_exists = to_attributes != INVALID_FILE_ATTRIBUTES;
  bool to_dir = to_exists && (to_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !to_link;
  int error = 0;
  bool same = to_exists && SameFile(h, to);
  // On a case-insensitive volume a new spelling of the same name is a rename.
  bool respelled = same && _wcsicmp(NtPath(from).c_str(), NtPath(to).c_str()) == 0 &&
                   NtPath(from) != NtPath(to);
  if (same && !respelled) {
    CloseHandle(h);
    return 0;  // two names of one file: nothing to do, as on Linux
  } else if (respelled) {
    // Nothing to replace.
  } else if (to_exists && (flags & kRenameNoReplace) != 0) {
    error = EEXIST;
  } else if (to_exists && from_dir && !to_dir) {
    error = ENOTDIR;
  } else if (to_exists && !from_dir && to_dir) {
    error = EISDIR;
  } else if (from_dir) {
    // A directory cannot move into itself.
    std::wstring a = NtPath(from), b = NtPath(to);
    if (b.size() > a.size() && _wcsnicmp(a.c_str(), b.c_str(), a.size()) == 0 &&
        b[a.size()] == L'\\') {
      error = EINVAL;
    } else if (to_dir) {
      if (!IsEmptyDirectory(to)) {
        error = ENOTEMPTY;
      } else if (gesher::PosixDelete(to, true) != 0) {
        error = errno;  // Windows replaces no directory: the empty one goes first
      }
    }
  }
  if (error != 0) {
    CloseHandle(h);
    return errno = error, -1;
  }
  std::wstring target = NtPath(to);
  size_t bytes = sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t);
  std::string storage(bytes, '\0');
  FILE_RENAME_INFO* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
  // FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS |
  // FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE, through FileRenameInfoEx (22).
  DWORD rename_flags = ((flags & kRenameNoReplace) != 0 ? 0 : 0x1) | 0x2 | 0x40;
  memcpy(rename, &rename_flags, sizeof(rename_flags));
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
  memcpy(rename->FileName, target.c_str(), (target.size() + 1) * sizeof(wchar_t));
  BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22), rename,
                                       static_cast<DWORD>(bytes));
  DWORD last = GetLastError();
  CloseHandle(h);
  if (!ok && last != ERROR_NOT_SAME_DEVICE && last != ERROR_ALREADY_EXISTS) {
    // Volumes without POSIX renames (FAT) or older Windows: the classic one.
    DWORD move = (flags & kRenameNoReplace) != 0 ? 0 : MOVEFILE_REPLACE_EXISTING;
    ok = MoveFileExW(from.c_str(), to.c_str(), move);
    last = GetLastError();
  }
  if (!ok) {
    SetLastError(last);
    return gesher::FailWithLastError();
  }
  return 0;
}

// Owner's write permission <-> FILE_ATTRIBUTE_READONLY, as stat reports it
// (stat.cc); directories keep theirs, as Windows ignores it there.
int SetMode(HANDLE h, mode_t mode) {
  FILE_BASIC_INFO basic;
  if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic))) {
    return gesher::FailWithLastError();
  }
  if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return 0;
  DWORD attributes = (mode & 0200) != 0 ? basic.FileAttributes & ~FILE_ATTRIBUTE_READONLY
                                        : basic.FileAttributes | FILE_ATTRIBUTE_READONLY;
  if (attributes == basic.FileAttributes) return 0;
  basic.FileAttributes = attributes == 0 ? FILE_ATTRIBUTE_NORMAL : attributes;
  // Times of 0 are left unchanged.
  basic.CreationTime.QuadPart = basic.LastAccessTime.QuadPart = 0;
  basic.LastWriteTime.QuadPart = basic.ChangeTime.QuadPart = 0;
  return SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof(basic))
             ? 0
             : gesher::FailWithLastError();
}

int Chmod(const char* path, mode_t mode, bool follow) {
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EPERM)) return -1;
  HANDLE h = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  int result = !follow && gesher::IsSymlinkHandle(h) ? (errno = EOPNOTSUPP, -1) : SetMode(h, mode);
  CloseHandle(h);
  return result;
}

int Access(const char* path, int mode, bool follow) {
  if ((mode & ~(R_OK | W_OK | X_OK)) != 0) return errno = EINVAL, -1;
  if (path == nullptr) return errno = EFAULT, -1;
  if (path[0] == '\0') return errno = ENOENT, -1;
  std::wstring windows;
  switch (gesher::MapPath(path, &windows)) {
    case PathKind::kFile:
      break;
    case PathKind::kNull:
    case PathKind::kZero:
    case PathKind::kRandom:
    case PathKind::kBinder:
      return (mode & X_OK) != 0 ? (errno = EACCES, -1) : 0;
    case PathKind::kVirtual: {
      unsigned type = gesher::procfs::Type(windows);
      if (type == 0) return errno = ENOENT, -1;
      if ((mode & W_OK) != 0 || ((mode & X_OK) != 0 && type != S_IFDIR)) return errno = EACCES, -1;
      return 0;
    }
    case PathKind::kMissing:
      return errno = ENOENT, -1;
  }
  HANDLE h = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  BY_HANDLE_FILE_INFORMATION info;
  bool ok = GetFileInformationByHandle(h, &info);
  CloseHandle(h);
  if (!ok) return gesher::FailWithLastError();
  // Read-only files are not writable. Windows has no execute permission: any
  // file may be run (an app's ELF executables through gesher-exec, docs/exec.md;
  // apps check File.canExecute() before they start one), so X_OK holds for
  // files as it does for directories.
  bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  bool read_only = !directory && (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  if ((mode & W_OK) != 0 && read_only) return errno = EACCES, -1;
  return 0;
}

// The symbolic link `linkpath` to `target` (a Linux path, relative or not).
int Symlink(const char* target, const char* linkpath) {
  if (target == nullptr) return errno = EFAULT, -1;
  if (target[0] == '\0') return errno = ENOENT, -1;
  std::wstring link;
  if (!gesher::DiskPath(linkpath, &link, EEXIST)) return -1;
  // The target as Windows stores it: backslashes; an absolute one with its
  // drive, as a drive-less one would be read against whatever drive is
  // current when the link is followed.
  std::wstring windows_target;
  bool absolute = target[0] == '/' || target[0] == '\\' ||
                  (((target[0] | 0x20) >= 'a' && (target[0] | 0x20) <= 'z') && target[1] == ':');
  if (absolute) {
    if (gesher::MapPath(target, &windows_target) != PathKind::kFile) return errno = EPERM, -1;
    DWORD n = GetFullPathNameW(windows_target.c_str(), 0, nullptr, nullptr);
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(windows_target.c_str(), n, full.data(), nullptr);
    if (n != 0 && n < full.size()) windows_target = full.substr(0, n);
  } else {
    windows_target = gesher::Widen(target);
  }
  for (wchar_t& c : windows_target) {
    if (c == L'/') c = L'\\';
  }
  // A link to a directory must say so; one to nothing is a file link.
  std::wstring resolved = windows_target;
  if (!absolute) {
    size_t slash = link.find_last_of(L"\\/");
    resolved = (slash == std::wstring::npos ? L"" : link.substr(0, slash + 1)) + windows_target;
  }
  DWORD attributes = GetFileAttributesW(resolved.c_str());
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE |
                (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                     ? SYMBOLIC_LINK_FLAG_DIRECTORY
                     : 0);
  if (!CreateSymbolicLinkW(link.c_str(), windows_target.c_str(), flags)) {
    // Without developer mode, creating symbolic links needs a privilege.
    if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD) return errno = EPERM, -1;
    return gesher::FailWithLastError();
  }
  return 0;
}

ssize_t Readlink(const char* path, char* buf, size_t size) {
  if (path == nullptr || buf == nullptr) return errno = EFAULT, -1;
  if (path[0] == '\0') return errno = ENOENT, -1;
  if (size == 0) return errno = EINVAL, -1;
  std::wstring windows;
  std::string target;
  switch (gesher::MapPath(path, &windows)) {
    case PathKind::kVirtual:
      if (!gesher::procfs::ReadLink(windows, &target)) return -1;
      break;
    case PathKind::kFile: {
      HANDLE h = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
      bool ok = gesher::IsSymlinkHandle(h) ? gesher::ReadSymlinkHandle(h, &target)
                                            : (errno = EINVAL, false);
      CloseHandle(h);
      if (!ok) return -1;
      break;
    }
    case PathKind::kMissing:
      return errno = ENOENT, -1;
    default:
      return errno = EINVAL, -1;  // devices are not links
  }
  size_t n = target.size() < size ? target.size() : size;  // not NUL-terminated
  memcpy(buf, target.data(), n);
  return static_cast<ssize_t>(n);
}

int Link(const char* oldpath, const char* newpath, bool follow) {
  std::wstring from, to;
  if (!gesher::DiskPath(oldpath, &from, EPERM) || !gesher::DiskPath(newpath, &to, EEXIST)) return -1;
  if (follow) {
    // AT_SYMLINK_FOLLOW: a link to the file a symbolic link names.
    HANDLE h = CreateFileW(from.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
    wchar_t final_path[32768];
    DWORD n = GetFinalPathNameByHandleW(h, final_path, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= 32768) return gesher::FailWithLastError();
    from = final_path;
  }
  bool link;
  DWORD attributes = AttributesNoFollow(from, &link);
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !link) {
    return errno = EPERM, -1;  // no hard links to directories, as on Linux
  }
  if (!CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) return gesher::FailWithLastError();
  return 0;
}

// fopen's mode as open flags and the CRT's stream mode. False for an invalid
// mode.
bool ParseMode(const char* mode, int* flags, char crt_mode[4]) {
  if (mode == nullptr) return false;
  int f;
  switch (mode[0]) {
    case 'r': f = O_RDONLY; break;
    case 'w': f = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': f = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return false;
  }
  bool plus = false;
  for (const char* p = mode + 1; *p != '\0'; ++p) {
    switch (*p) {
      case '+': plus = true; break;
      case 'x': f |= O_EXCL; break;
      case 'e': f |= O_CLOEXEC; break;
      case 'b':
      case 't':  // Linux has no text mode
        break;
      default:
        break;  // glibc ignores what it does not know (",ccs=" included)
    }
  }
  if (plus) f = (f & ~O_ACCMODE) | O_RDWR;
  *flags = f;
  crt_mode[0] = mode[0];
  crt_mode[1] = plus ? '+' : 'b';
  crt_mode[2] = plus ? 'b' : '\0';
  crt_mode[3] = '\0';
  return true;
}

// Random characters in place of a template's six trailing Xs (and
// `suffix` characters after them), until `create` stops failing with EEXIST.
template <typename Create>
bool FillTemplate(char* name, int suffix, Create create) {
  size_t length = name != nullptr ? strlen(name) : 0;
  if (suffix < 0 || length < 6 + static_cast<size_t>(suffix) ||
      strncmp(name + length - suffix - 6, "XXXXXX", 6) != 0) {
    return errno = EINVAL, false;
  }
  static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  char* x = name + length - suffix - 6;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    unsigned char random[6];
    BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    for (int i = 0; i < 6; ++i) x[i] = kChars[random[i] % (sizeof(kChars) - 1)];
    if (create(name)) return true;
    if (errno != EEXIST) return false;
  }
  return errno = EEXIST, false;
}

int MakeTemp(char* name, int suffix, int flags) {
  int fd = -1;
  if (!FillTemplate(name, suffix, [&](const char* path) {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | (flags & ~O_ACCMODE), 0600);
        return fd >= 0;
      })) {
    return -1;
  }
  return fd;
}

}  // namespace

extern "C" {

int access(const char* path, int mode) { return Access(path, mode, true); }

int faccessat(int dirfd, const char* path, int mode, int flags) {
  if ((flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0) return errno = EINVAL, -1;
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  // One uid for real and effective ids: AT_EACCESS changes nothing.
  return Access(resolved.c_str(), mode, (flags & AT_SYMLINK_NOFOLLOW) == 0);
}

int rename(const char* oldpath, const char* newpath) { return Rename(oldpath, newpath, 0); }

int renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
              unsigned int flags) {
  std::string from, to;
  if (!gesher::ResolveAt(olddirfd, oldpath, &from) || !gesher::ResolveAt(newdirfd, newpath, &to)) {
    return -1;
  }
  return Rename(from.c_str(), to.c_str(), flags);
}

int renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
  return renameat2(olddirfd, oldpath, newdirfd, newpath, 0);
}

int remove(const char* path) {
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EACCES)) return -1;
  bool link;
  DWORD attributes = AttributesNoFollow(windows, &link);
  bool directory = attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !link;
  return gesher::PosixDelete(windows, directory);
}

int unlinkat(int dirfd, const char* path, int flags) {
  if ((flags & ~AT_REMOVEDIR) != 0) return errno = EINVAL, -1;
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return (flags & AT_REMOVEDIR) != 0 ? rmdir(resolved.c_str()) : unlink(resolved.c_str());
}

// A short directory is made current without the \\?\ prefix, so that
// drive-less paths resolve against its drive as usual.
static int SetCurrent(std::wstring path) {
  if (path.size() < MAX_PATH + 4) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
      path = L"\\\\" + path.substr(8);
    } else if (path.rfind(L"\\\\?\\", 0) == 0) {
      path = path.substr(4);
    }
  }
  return SetCurrentDirectoryW(path.c_str()) ? 0 : gesher::FailWithLastError();
}

int chdir(const char* path) {
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, ENOTDIR)) return -1;
  return SetCurrent(windows);
}

int fchdir(int fd) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  wchar_t path[32768];
  DWORD n = GetFinalPathNameByHandleW(h, path, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) return errno = ENOTDIR, -1;
  return SetCurrent(path);
}

int chmod(const char* path, int mode) { return Chmod(path, static_cast<mode_t>(mode), true); }

int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
  if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) return errno = EINVAL, -1;
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return Chmod(resolved.c_str(), mode, (flags & AT_SYMLINK_NOFOLLOW) == 0);
}

// Linux lets the owner change the mode through any fd, so this works on a
// second handle with the access it needs (as futimens, path.cc).
int fchmod(int fd, mode_t mode) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (GetFileType(h) != FILE_TYPE_DISK) return 0;  // pipes, sockets, devices
  HANDLE writable = ReOpenFile(h, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               FILE_FLAG_BACKUP_SEMANTICS);
  if (writable == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  int result = SetMode(writable, mode);
  CloseHandle(writable);
  return result;
}

int symlink(const char* target, const char* linkpath) { return Symlink(target, linkpath); }

int symlinkat(const char* target, int newdirfd, const char* linkpath) {
  std::string resolved;
  if (!gesher::ResolveAt(newdirfd, linkpath, &resolved)) return -1;
  return Symlink(target, resolved.c_str());
}

ssize_t readlink(const char* path, char* buf, size_t size) { return Readlink(path, buf, size); }

ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t size) {
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return Readlink(resolved.c_str(), buf, size);
}

int link(const char* oldpath, const char* newpath) { return Link(oldpath, newpath, false); }

int linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags) {
  if ((flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) != 0) return errno = EINVAL, -1;
  std::string from, to;
  if (!gesher::ResolveAt(olddirfd, oldpath, &from) || !gesher::ResolveAt(newdirfd, newpath, &to)) {
    return -1;
  }
  return Link(from.c_str(), to.c_str(), (flags & AT_SYMLINK_FOLLOW) != 0);
}

// Streams over gesher-base's open, so they get its paths and Linux's
// sharing (an open file can be renamed or unlinked); always binary.
FILE* fopen(const char* path, const char* mode) {
  int flags;
  char crt_mode[4];
  if (!ParseMode(mode, &flags, crt_mode)) {
    errno = EINVAL;
    return nullptr;
  }
  int fd = open(path, flags, 0666);
  if (fd < 0) return nullptr;
  FILE* stream = _fdopen(fd, crt_mode);
  if (stream == nullptr) {
    int saved = gesher_errno_from_crt(errno);
    close(fd);
    errno = saved;
  }
  return stream;
}

FILE* fopen64(const char* path, const char* mode) { return fopen(path, mode); }

int fclose(FILE* stream) {
  gesher::record_locks::ClosingDescriptor transaction(_fileno(stream));
  if (transaction.Error()) return errno = transaction.Error(), EOF;
  // Use the CRT's documented unlocked entry under its stream lock, avoiding
  // recursion through this public fclose symbol.
  _lock_file(stream);
  int result = _fclose_nolock(stream);
  _unlock_file(stream);
  int saved = result == EOF ? gesher_errno_from_crt(errno) : errno;
  int error = transaction.Finish();
  if (error) return errno = error, EOF;
  errno = saved;
  return result;
}

// The stream is reopened by the CRT on the Windows name (NUL for /dev/null),
// keeping its FILE.
FILE* freopen(const char* path, const char* mode, FILE* stream) {
  int flags;
  char crt_mode[4];
  if (!ParseMode(mode, &flags, crt_mode)) {
    errno = EINVAL;
    return nullptr;
  }
  if (path == nullptr) return stream;  // a mode change: the CRT cannot; Linux may refuse too
  std::wstring windows;
  switch (path[0] != '\0' ? gesher::MapPath(path, &windows) : PathKind::kMissing) {
    case PathKind::kFile:
      break;
    case PathKind::kNull:
      windows = L"NUL";
      break;
    case PathKind::kMissing:
      errno = ENOENT;
      return nullptr;
    default:
      errno = EACCES;
      return nullptr;
  }
  std::wstring wmode(crt_mode, crt_mode + strlen(crt_mode));
  gesher::record_locks::ClosingDescriptor transaction(_fileno(stream));
  if (transaction.Error()) return errno = transaction.Error(), nullptr;
  FILE* result = _wfreopen(windows.c_str(), wmode.c_str(), stream);
  int saved = result == nullptr ? gesher_errno_from_crt(errno) : errno;
  int error = transaction.Finish();
  if (error) return errno = error, nullptr;
  errno = saved;
  return result;
}

FILE* freopen64(const char* path, const char* mode, FILE* stream) {
  return freopen(path, mode, stream);
}

char* mkdtemp(char* name) {
  return FillTemplate(name, 0, [](const char* path) { return mkdir(path, 0700) == 0; }) ? name
                                                                                       : nullptr;
}

int mkstemp(char* name) { return MakeTemp(name, 0, 0); }
int mkostemp(char* name, int flags) { return MakeTemp(name, 0, flags); }
int mkstemps(char* name, int suffix) { return MakeTemp(name, suffix, 0); }
int mkostemps(char* name, int suffix, int flags) { return MakeTemp(name, suffix, flags); }

}  // extern "C"
