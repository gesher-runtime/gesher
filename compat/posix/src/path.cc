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

namespace gesher {
namespace {

bool IsDriveLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Linux's absolute paths start with '/'; Windows' with a drive or '\'.
bool IsAbsolute(const char* path) {
  return path[0] == '/' || path[0] == '\\' || (IsDriveLetter(path[0]) && path[1] == ':');
}

// /dev, /proc and /sys are Linux's virtual file systems: their names never
// reach the disk. Returns kFile for any other path.
PathKind VirtualKind(std::string_view path) {
  std::vector<std::string_view> parts;
  while (!path.empty()) {
    size_t end = path.find('/');
    std::string_view part = path.substr(0, end);
    if (!part.empty() && part != ".") parts.push_back(part);
    if (end == std::string_view::npos) break;
    path.remove_prefix(end + 1);
  }
  if (parts.empty() || (parts[0] != "dev" && parts[0] != "proc" && parts[0] != "sys")) {
    return PathKind::kFile;
  }
  if (parts.size() == 2 && parts[0] == "dev") {
    if (parts[1] == "null") return PathKind::kNull;
    if (parts[1] == "zero") return PathKind::kZero;
    if (parts[1] == "random" || parts[1] == "urandom") return PathKind::kRandom;
    if (parts[1] == "binder") return BinderDeviceExists() ? PathKind::kBinder : PathKind::kMissing;
  }
  if (parts.size() == 3 && parts[0] == "proc" && parts[2] == "cmdline" &&
      (parts[1] == "self" || parts[1] == std::to_string(GetCurrentProcessId()))) {
    return PathKind::kCmdline;
  }
  return PathKind::kMissing;
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
  int crt_flags = _O_BINARY | ((flags & O_APPEND) != 0 ? _O_APPEND : 0) |
                  ((flags & O_ACCMODE) == O_RDONLY ? _O_RDONLY : 0) |
                  ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0);
  int fd = CrtResult(_open_osfhandle(reinterpret_cast<intptr_t>(h), crt_flags));
  if (fd < 0) CloseHandle(h);
  return fd;
}

int OpenFile(const std::wstring& path, int flags) {
  if ((flags & O_TMPFILE) == O_TMPFILE) return errno = EOPNOTSUPP, -1;
  DWORD disposition;
  if ((flags & O_PATH) != 0) {
    disposition = OPEN_EXISTING;
  } else if ((flags & O_CREAT) != 0) {
    disposition = (flags & O_EXCL) != 0 ? CREATE_NEW : (flags & O_TRUNC) != 0 ? CREATE_ALWAYS : OPEN_ALWAYS;
  } else {
    disposition = (flags & O_TRUNC) != 0 ? TRUNCATE_EXISTING : OPEN_EXISTING;
  }
  // Backup semantics open directories too, as Linux does for reading.
  DWORD attributes = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS |
                     ((flags & O_DSYNC) != 0 ? FILE_FLAG_WRITE_THROUGH : 0);
  SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, (flags & O_CLOEXEC) == 0};
  DWORD access = Access(flags);
  HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         &inherit, disposition, attributes, nullptr);
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

// /proc/self/cmdline: the arguments, each followed by a NUL, in an anonymous
// file.
int OpenCmdline(int flags) {
  if ((flags & O_ACCMODE) != O_RDONLY) return errno = EACCES, -1;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return FailWithLastError();
  std::string content;
  for (int i = 0; i < argc; ++i) {
    content += Narrow(argv[i]);
    content += '\0';
  }
  LocalFree(argv);
  int fd = memfd_create("cmdline", (flags & O_CLOEXEC) != 0 ? MFD_CLOEXEC : 0);
  if (fd < 0) return -1;
  if (_write(fd, content.data(), static_cast<unsigned>(content.size())) !=
          static_cast<int>(content.size()) ||
      _lseeki64(fd, 0, SEEK_SET) != 0) {
    int saved = gesher_errno_from_crt(errno);
    _close(fd);
    return errno = saved, -1;
  }
  return fd;
}

int OpenPath(const char* path, int flags) {
  if (path == nullptr) return errno = EFAULT, -1;
  if (*path == '\0') return errno = ENOENT, -1;
  std::wstring windows;
  PathKind kind = MapPath(path, &windows);
  switch (kind) {
    case PathKind::kFile: return OpenFile(windows, flags);
    case PathKind::kNull:
    case PathKind::kZero:
    case PathKind::kRandom: return OpenDevice(kind, flags);
    case PathKind::kCmdline: return OpenCmdline(flags);
    case PathKind::kBinder: return OpenBinder(flags);
    case PathKind::kMissing: break;
  }
  return errno = (flags & O_CREAT) != 0 ? EACCES : ENOENT, -1;
}

}  // namespace

PathKind MapPath(const char* path, std::wstring* windows) {
  // "//name" is left to Windows as a UNC path; POSIX lets an implementation
  // give a leading "//" its own meaning.
  if (path[0] == '/' && path[1] != '/') {
    PathKind kind = VirtualKind(path);
    if (kind != PathKind::kFile) return kind;
  }
  *windows = Widen(path);
  Lengthen(windows);
  return PathKind::kFile;
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
  // _getdrive is 0 when the current directory is a UNC path.
  int drive = _getdrive();
  if (drive > 0 && path.size() >= 2 && path[1] == L':' && (path.size() == 2 || path[2] == L'/') &&
      towupper(path[0]) == static_cast<wchar_t>(L'A' + drive - 1)) {
    path.erase(0, 2);
    if (path.empty()) path = L"/";
  }
  return Narrow(path.c_str());
}

}  // namespace gesher

using gesher::PathKind;

extern "C" {

int open(const char* path, int flags, ...) {
  // The mode is not used: permissions are not modelled.
  return gesher::OpenPath(path, flags);
}

int openat(int dirfd, const char* path, int flags, ...) {
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return gesher::OpenPath(resolved.c_str(), flags);
}

// stat, fstat and lstat are in stat.cc.

int fstatat(int dirfd, const char* path, struct stat* buf, int flags) {
  if ((flags & AT_EMPTY_PATH) != 0 && path != nullptr && path[0] == '\0') return fstat(dirfd, buf);
  std::string resolved;
  if (!gesher::ResolveAt(dirfd, path, &resolved)) return -1;
  return stat(resolved.c_str(), buf);
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
