// gesher-base: Linux paths on Windows (docs/decisions/0010-paths.md), and
// the calls where paths enter: open, openat, fstatat, mkdir, mkdirat,
// getcwd. stat (stat.cc) maps its path with MapPath too.
//
// open, mkdir and getcwd are defined here under the CRT's own names, like
// close and read in fd.cc: gesher-base is linked before the CRT, so every
// caller gets these. open takes Linux's O_* values
// (<fcntl.h>) and opens files with CreateFileW, sharing delete access as
// Linux allows unlinking an open file.
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>
#include <vector>

#include "binder_internal.h"
#include "internal.h"
#include "record_lock_descriptors.h"

namespace gesher {
namespace {

bool IsDriveLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Linux's absolute paths start with '/'; Windows' with a drive or '\'.
bool IsAbsolute(const char* path) {
  return path[0] == '/' || path[0] == '\\' || (IsDriveLetter(path[0]) && path[1] == ':');
}

// The parts of an absolute path, without empty and "." components.
std::vector<std::string_view> Parts(std::string_view path) {
  std::vector<std::string_view> parts;
  while (!path.empty()) {
    size_t end = path.find('/');
    std::string_view part = path.substr(0, end);
    if (!part.empty() && part != ".") parts.push_back(part);
    if (end == std::string_view::npos) break;
    path.remove_prefix(end + 1);
  }
  return parts;
}

// /dev, /proc and /sys are Linux's virtual file systems: their names never
// reach the disk; nor does /etc/timezone, which libcore reads for the default
// time zone. Returns kFile for any other path. For kVirtual, *name is the
// normalized name ("proc/self/status").
PathKind VirtualKind(std::string_view path, std::wstring* name) {
  std::vector<std::string_view> parts = Parts(path);
  if (parts.empty()) return PathKind::kFile;
  if (parts.size() == 2 && parts[0] == "etc" && parts[1] == "timezone") {
    *name = L"etc/timezone";
    return PathKind::kVirtual;
  }
  if (parts[0] == "proc") {
    std::string joined;
    for (std::string_view part : parts) {
      if (!joined.empty()) joined += '/';
      joined += part;
    }
    *name = Widen(joined.c_str());
    return PathKind::kVirtual;
  }
  if (parts[0] != "dev" && parts[0] != "sys") return PathKind::kFile;
  if (parts.size() == 2 && parts[0] == "dev") {
    if (parts[1] == "null") return PathKind::kNull;
    if (parts[1] == "zero") return PathKind::kZero;
    if (parts[1] == "random" || parts[1] == "urandom") return PathKind::kRandom;
    if (parts[1] == "binder") return BinderDeviceExists() ? PathKind::kBinder : PathKind::kMissing;
  }
  return PathKind::kMissing;
}

// /mnt/<drive>[/...] is that drive's path (ADR 0045): "D:\..." for "/mnt/d/...".
// False if `path` is not such a name.
bool DrivePath(const char* path, std::wstring* windows) {
  if (strncmp(path, "/mnt/", 5) != 0 || !IsDriveLetter(path[5]) ||
      (path[6] != '\0' && path[6] != '/')) {
    return false;
  }
  std::wstring rest = Widen(path + 6);
  for (wchar_t& c : rest) {
    if (c == L'/') c = L'\\';
  }
  *windows = std::wstring(1, static_cast<wchar_t>(towupper(path[5]))) + L":" +
             (rest.empty() ? L"\\" : rest);
  return true;
}

// Paths from MAX_PATH on need the \\?\ form, which takes only full paths
// with backslashes. (CreateDirectoryW stops 12 characters short of it.)
void Lengthen(std::wstring* path) {
  if (path->size() < MAX_PATH - 12 || path->rfind(L"\\\\?\\", 0) == 0) return;
  DWORD n = GetFullPathNameW(path->c_str(), 0, nullptr, nullptr);
  if (n == 0) return;
  std::wstring full(n, L'\0');
  n = GetFullPathNameW(path->c_str(), n, full.data(), nullptr);
  if (n == 0 || n >= full.size()) return;
  full.resize(n);
  *path = full.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + full.substr(2) : L"\\\\?\\" + full;
}

DWORD Access(int flags) {
  if ((flags & O_PATH) != 0) return 0;
  DWORD access = 0;
  switch (flags & O_ACCMODE) {
    case O_RDONLY: access = GENERIC_READ; break;
    case O_WRONLY: access = GENERIC_WRITE; break;
    default: access = GENERIC_READ | GENERIC_WRITE; break;
  }
  return (flags & O_TRUNC) != 0 ? access | GENERIC_WRITE : access;
}

// The CRT fd for a new handle; closes the handle if there is none.
int FdFromHandle(HANDLE h, int flags) {
  record_locks::DescriptorGuard descriptor_guard;
  int crt_flags = _O_BINARY | ((flags & O_APPEND) != 0 ? _O_APPEND : 0) |
                  ((flags & O_ACCMODE) == O_RDONLY ? _O_RDONLY : 0) |
                  ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0);
  int fd = CrtResult(_open_osfhandle(reinterpret_cast<intptr_t>(h), crt_flags));
  if (fd < 0) CloseHandle(h);
  return fd;
}

int OpenFile(const std::wstring& path, int flags, mode_t mode) {
  if ((flags & O_TMPFILE) == O_TMPFILE) return errno = EOPNOTSUPP, -1;
  DWORD disposition;
  if ((flags & O_PATH) != 0) {
    disposition = OPEN_EXISTING;
  } else if ((flags & O_CREAT) != 0) {
    disposition = (flags & O_EXCL) != 0 ? CREATE_NEW : (flags & O_TRUNC) != 0 ? CREATE_ALWAYS : OPEN_ALWAYS;
  } else {
    disposition = (flags & O_TRUNC) != 0 ? TRUNCATE_EXISTING : OPEN_EXISTING;
  }
  // Backup semantics open directories too, as Linux does for reading. A new
  // file without the owner's write permission is read-only, as chmod makes
  // it (fsops.cc); the fd itself may still write, as on Linux. O_NOFOLLOW
  // opens a symbolic link itself, to refuse it below (or, with O_PATH, to
  // return it).
  DWORD attributes = FILE_FLAG_BACKUP_SEMANTICS |
                     ((flags & O_CREAT) != 0 && (mode & 0200) == 0 ? FILE_ATTRIBUTE_READONLY
                                                                   : FILE_ATTRIBUTE_NORMAL) |
                     ((flags & O_NOFOLLOW) != 0 ? FILE_FLAG_OPEN_REPARSE_POINT : 0) |
                     ((flags & O_DSYNC) != 0 ? FILE_FLAG_WRITE_THROUGH : 0);
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, (flags & O_CLOEXEC) == 0};
  DWORD access = Access(flags);
  HANDLE h = INVALID_HANDLE_VALUE;
  // A brief open that denies sharing (a scanner, inotify.cc's check for
  // writers) is waited out.
  for (int attempt = 0; attempt < 20; ++attempt) {
    h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    &inherit, disposition, attributes, nullptr);
    if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
    Sleep(1);
  }
  if (h == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    DWORD existing = GetFileAttributesW(path.c_str());
    if (error == ERROR_ACCESS_DENIED && existing != INVALID_FILE_ATTRIBUTES &&
        (existing & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return errno = EISDIR, -1;
    }
    SetLastError(error);
    return FailWithLastError();
  }
  if ((flags & (O_NOFOLLOW | O_PATH)) == O_NOFOLLOW && IsSymlinkHandle(h)) {
    CloseHandle(h);
    return errno = ELOOP, -1;
  }
  BY_HANDLE_FILE_INFORMATION info;
  bool directory = GetFileInformationByHandle(h, &info) &&
                   (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((flags & O_DIRECTORY) != 0 && !directory) {
    CloseHandle(h);
    return errno = ENOTDIR, -1;
  }
  if (directory && (access & GENERIC_WRITE) != 0) {
    CloseHandle(h);
    return errno = EISDIR, -1;
  }
  return FdFromHandle(h, flags);
}

// /dev/null, and /dev/zero and /dev/random over it.
int OpenDevice(PathKind kind, int flags) {
  if ((flags & O_DIRECTORY) != 0) return errno = ENOTDIR, -1;
  if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return errno = EEXIST, -1;
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, (flags & O_CLOEXEC) == 0};
  HANDLE h = CreateFileW(L"NUL", Access(flags), FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return FailWithLastError();
  int fd = FdFromHandle(h, flags);
  if (fd >= 0 && kind == PathKind::kZero) RegisterDevice(fd, Device::kZero);
  if (fd >= 0 && kind == PathKind::kRandom) RegisterDevice(fd, Device::kRandom);
  return fd;
}

int OpenPath(const char* path, int flags, mode_t mode) {
  if (path == nullptr) return errno = EFAULT, -1;
  if (*path == '\0') return errno = ENOENT, -1;
  std::wstring windows;
  PathKind kind = MapPath(path, &windows);
  switch (kind) {
    case PathKind::kFile: return OpenFile(windows, flags, mode);
    case PathKind::kNull:
    case PathKind::kZero:
    case PathKind::kRandom: return OpenDevice(kind, flags);
    case PathKind::kVirtual: return procfs::Open(windows, flags);
    case PathKind::kBinder: return OpenBinder(flags);
    case PathKind::kMissing: break;
  }
  return errno = (flags & O_CREAT) != 0 ? EACCES : ENOENT, -1;
}

}  // namespace

// Joins a path relative to dirfd onto that directory's path; absolute paths
// and AT_FDCWD pass unchanged. False (errno set) if dirfd is not a directory.
bool ResolveAt(int dirfd, const char* path, std::string* out) {
  if (path == nullptr) return errno = EFAULT, false;
  if (dirfd == AT_FDCWD || IsAbsolute(path)) {
    *out = path;
    return true;
  }
  HANDLE h = HandleFromFd(dirfd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, false;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return errno = ENOTDIR, false;
  }
  DWORD n = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  std::wstring dir(n, L'\0');
  n = GetFinalPathNameByHandleW(h, dir.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= dir.size()) return FailWithLastError(), false;
  dir.resize(n);
  *out = LinuxPath(dir.c_str());
  if (out->back() != '/') *out += '/';
  *out += path;
  return true;
}

PathKind MapPath(const char* path, std::wstring* windows) {
  // "//name" is left to Windows as a UNC path; POSIX lets an implementation
  // give a leading "//" its own meaning.
  if (path[0] == '/' && path[1] != '/') {
    PathKind kind = VirtualKind(path, windows);
    if (kind != PathKind::kFile) return kind;
    if (DrivePath(path, windows)) {
      Lengthen(windows);
      return PathKind::kFile;
    }
  }
  *windows = Widen(path);
  Lengthen(windows);
  return PathKind::kFile;
}

bool DiskPath(const char* path, std::wstring* windows, int special) {
  if (path == nullptr) return errno = EFAULT, false;
  if (path[0] == '\0') return errno = ENOENT, false;
  switch (MapPath(path, windows)) {
    case PathKind::kFile:
      return true;
    case PathKind::kMissing:
      return errno = ENOENT, false;
    case PathKind::kVirtual:
      return errno = procfs::Type(*windows) == 0 ? ENOENT : special, false;
    default:
      return errno = special, false;
  }
}

// The current directory's drive, 1 for A:; 0 when it has none (UNC). Unlike
// _getdrive, it also reads a \\?\ current directory.
int CurrentDrive() {
  wchar_t cwd[8];
  DWORD n = GetCurrentDirectoryW(8, cwd);
  std::wstring head;
  if (n >= 8) {
    std::wstring full(n, L'\0');
    n = GetCurrentDirectoryW(n, full.data());
    head = full.substr(0, n);
  } else {
    head.assign(cwd, n);
  }
  if (head.rfind(L"\\\\?\\", 0) == 0) head = head.substr(4);
  if (head.size() >= 2 && head[1] == L':' && iswalpha(head[0])) return towupper(head[0]) - L'A' + 1;
  return 0;
}

std::string LinuxPath(const wchar_t* windows) {
  std::wstring path = windows;
  if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    path = L"\\\\" + path.substr(8);
  } else if (path.rfind(L"\\\\?\\", 0) == 0) {
    path = path.substr(4);
  }
  for (wchar_t& c : path) {
    if (c == L'\\') c = L'/';
  }
  int drive = CurrentDrive();
  if (path.size() >= 2 && path[1] == L':' && iswalpha(path[0]) && (path.size() == 2 || path[2] == L'/')) {
    if (drive > 0 && towupper(path[0]) == static_cast<wchar_t>(L'A' + drive - 1)) {
      path.erase(0, 2);
      if (path.empty()) path = L"/";
    } else {
      // Another drive: /mnt/<drive>, so the path needs no colon either.
      std::wstring rest = path.substr(2);
      if (rest == L"/") rest.clear();
      path = std::wstring(L"/mnt/") + static_cast<wchar_t>(towlower(path[0])) + rest;
    }
  }
  return Narrow(path.c_str());
}

// Linux's unlink and rmdir remove the name at once, even while the file is
// open; Windows' DeleteFile and RemoveDirectory may leave it in place (delete
// pending) until the last handle closes. The POSIX-semantics delete gives
// Linux's behaviour where the volume has it, the classic one otherwise; a
// scanner's brief open of a new file is waited out. A symbolic link is
// removed itself, by unlink, also when it points to a directory.
int PosixDelete(const std::wstring& windows, bool directory) {
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 20; ++attempt) {
    h = CreateFileW(windows.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) break;
    Sleep(10);
  }
  if (h == INVALID_HANDLE_VALUE) return FailWithLastError();
  FILE_BASIC_INFO basic;
  bool is_directory = GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic)) &&
                      (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                      !IsSymlinkHandle(h);
  if (is_directory != directory) {
    CloseHandle(h);
    return errno = directory ? ENOTDIR : EISDIR, -1;
  }
  FILE_DISPOSITION_INFO_EX posix = {FILE_DISPOSITION_FLAG_DELETE |
                                    FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                    FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
  // FileDispositionInfoEx (21), which older SDK headers declare only for
  // newer _WIN32_WINNT values.
  BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(21), &posix,
                                       sizeof(posix));
  DWORD error = GetLastError();
  if (!ok && error != ERROR_DIR_NOT_EMPTY) {
    // Linux removes read-only files; the classic delete does not.
    if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
      basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
      SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof(basic));
    }
    FILE_DISPOSITION_INFO classic = {TRUE};
    ok = SetFileInformationByHandle(h, FileDispositionInfo, &classic, sizeof(classic));
    error = GetLastError();
  }
  CloseHandle(h);
  if (!ok) {
    SetLastError(error);
    return FailWithLastError();
  }
  return 0;
}

}  // namespace gesher

using gesher::PathKind;

extern "C" {

// Of the mode, only the owner's write permission is kept: without it a new
// file is read-only.
int open(const char* path, int flags, ...) {
  mode_t mode = 0666;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = static_cast<mode_t>(va_arg(args, int));
    va_end(args);
  }
  return gesher::OpenPath(path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
  mode_t mode = 0666;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = static_cast<mode_t>(va_arg(args, int));
    va_end(args);
  }
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return gesher::OpenPath(resolved.c_str(), flags, mode);
}

int creat(const char* path, int mode) {
  return gesher::OpenPath(path, O_CREAT | O_WRONLY | O_TRUNC, static_cast<mode_t>(mode));
}

// stat, fstat and lstat are in stat.cc.

int fstatat(int dirfd, const char* path, struct stat* buf, int flags) {
  if ((flags & AT_EMPTY_PATH) != 0 && path != nullptr && path[0] == '\0') return fstat(dirfd, buf);
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return (flags & AT_SYMLINK_NOFOLLOW) != 0 ? lstat(resolved.c_str(), buf)
                                            : stat(resolved.c_str(), buf);
}

// --- file times ---------------------------------------------------------------

namespace {

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr long long kEpochDifference = 116444736000000000LL;

// Sets access and modification times (times[0], times[1]) as utimensat
// does: null means now, as do UTIME_NOW entries; UTIME_OMIT leaves one as
// it is. The handle needs FILE_WRITE_ATTRIBUTES.
int SetTimes(HANDLE h, const struct timespec times[2]) {
  FILETIME now;
  GetSystemTimePreciseAsFileTime(&now);
  FILETIME values[2];
  FILETIME* set[2];
  for (int i = 0; i < 2; ++i) {
    if (times == nullptr || times[i].tv_nsec == UTIME_NOW) {
      values[i] = now;
      set[i] = &values[i];
    } else if (times[i].tv_nsec == UTIME_OMIT) {
      set[i] = nullptr;
    } else {
      if (times[i].tv_nsec < 0 || times[i].tv_nsec >= 1000000000) return errno = EINVAL, -1;
      long long t = static_cast<long long>(times[i].tv_sec) * 10000000 +
                    times[i].tv_nsec / 100 + kEpochDifference;
      if (t < 0) return errno = EINVAL, -1;  // before 1601
      values[i].dwLowDateTime = static_cast<DWORD>(t);
      values[i].dwHighDateTime = static_cast<DWORD>(static_cast<unsigned long long>(t) >> 32);
      set[i] = &values[i];
    }
  }
  return SetFileTime(h, nullptr, set[0], set[1]) ? 0 : gesher::FailWithLastError();
}

bool ToTimespecs(const struct timeval tv[2], struct timespec ts[2]) {
  for (int i = 0; i < 2; ++i) {
    if (tv[i].tv_usec < 0 || tv[i].tv_usec >= 1000000) return errno = EINVAL, false;
    ts[i].tv_sec = tv[i].tv_sec;
    ts[i].tv_nsec = tv[i].tv_usec * 1000;
  }
  return true;
}

}  // namespace

// Symbolic links are followed even with AT_SYMLINK_NOFOLLOW, as by lstat
// (stat.cc).
int utimensat(int dirfd, const char* path, const struct timespec times[2], int) {
  if (path == nullptr) return futimens(dirfd, times);  // Linux: the fd itself
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  if (resolved.empty()) return errno = ENOENT, -1;
  std::wstring windows;
  switch (gesher::MapPath(resolved.c_str(), &windows)) {
    case PathKind::kFile: {
      HANDLE h = CreateFileW(windows.c_str(), FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
      int result = SetTimes(h, times);
      DWORD error = GetLastError();
      CloseHandle(h);
      SetLastError(error);
      return result;
    }
    case PathKind::kMissing:
      return errno = ENOENT, -1;
    default:
      return errno = EPERM, -1;  // devices and /proc files
  }
}

// Linux lets the owner set times through any fd, a read-only one included,
// so this works on a second handle with the access it needs.
int futimens(int fd, const struct timespec times[2]) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (GetFileType(h) != FILE_TYPE_DISK) return 0;  // pipes, sockets, devices
  HANDLE writable = ReOpenFile(h, FILE_WRITE_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
  if (writable == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  int result = SetTimes(writable, times);
  CloseHandle(writable);
  return result;
}

int utimes(const char* path, const struct timeval tv[2]) {
  struct timespec ts[2];
  if (tv != nullptr && !ToTimespecs(tv, ts)) return -1;
  return utimensat(AT_FDCWD, path, tv != nullptr ? ts : nullptr, 0);
}

int lutimes(const char* path, const struct timeval tv[2]) { return utimes(path, tv); }

int futimes(int fd, const struct timeval tv[2]) {
  struct timespec ts[2];
  if (tv != nullptr && !ToTimespecs(tv, ts)) return -1;
  return futimens(fd, tv != nullptr ? ts : nullptr);
}

int mkdir(const char* path) {
  if (path == nullptr) return errno = EFAULT, -1;
  std::wstring windows;
  switch (path[0] != '\0' ? gesher::MapPath(path, &windows) : PathKind::kMissing) {
    case PathKind::kFile:
      return CreateDirectoryW(windows.c_str(), nullptr) ? 0 : gesher::FailWithLastError();
    case PathKind::kMissing:
      return errno = ENOENT, -1;
    default:
      return errno = EEXIST, -1;
  }
}

int unlink(const char* path) {
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EACCES)) return -1;  // EACCES: /dev and /proc names
  return gesher::PosixDelete(windows, false);
}

int mkdirat(int dirfd, const char* path, mode_t) {
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return mkdir(resolved.c_str());
}

// Linux paths, without the drive when it is the current one, so that the
// result can go in a ':'-separated list. A null buf is allocated (glibc).
char* getcwd(char* buf, int size) {
  DWORD n = GetCurrentDirectoryW(0, nullptr);
  std::wstring cwd(n, L'\0');
  n = GetCurrentDirectoryW(n, cwd.data());
  if (n == 0 || n >= cwd.size()) {
    gesher::FailWithLastError();
    return nullptr;
  }
  cwd.resize(n);
  std::string path = gesher::LinuxPath(cwd.c_str());
  size_t needed = path.size() + 1;
  if (buf == nullptr) {
    if (size > 0 && needed > static_cast<size_t>(size)) return errno = ERANGE, nullptr;
    return _strdup(path.c_str());
  }
  if (size <= 0) return errno = EINVAL, nullptr;
  if (needed > static_cast<size_t>(size)) return errno = ERANGE, nullptr;
  memcpy(buf, path.c_str(), needed);
  return buf;
}

}  // extern "C"
