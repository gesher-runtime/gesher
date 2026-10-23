// gesher-base internals shared by the implementation files.
#pragma once

#include <errno.h>
#include <windows.h>

#include <string>

namespace gesher {

// POSIX paths are UTF-8; Windows APIs take UTF-16.
std::wstring Widen(const char* utf8);
std::string Narrow(const wchar_t* utf16);

// Maps a Win32 error code to the closest errno value.
int ErrnoFromWin32(DWORD error);

// Sets errno from GetLastError() and returns -1.
inline int FailWithLastError() {
  errno = ErrnoFromWin32(GetLastError());
  return -1;
}

// Passes on a CRT call's result, translating the errno it set on failure
// to Linux's numbering (see <errno.h>).
inline int CrtResult(int result) {
  if (result < 0) errno = gesher_errno_from_crt(errno);
  return result;
}

// The Win32 handle behind a CRT file descriptor, or INVALID_HANDLE_VALUE.
HANDLE HandleFromFd(int fd);

// Where a Linux path leads on Windows (path.cc,
// docs/decisions/0010-paths.md).
enum class PathKind {
  kFile,     // a file system path, in `windows`
  kNull,     // /dev/null
  kZero,     // /dev/zero
  kRandom,   // /dev/random, /dev/urandom
  kCmdline,  // /proc/self/cmdline
  kMissing,  // any other name under /dev, /proc or /sys
};
PathKind MapPath(const char* path, std::wstring* windows);

// A Windows path as Linux code sees it: '/' separators, and no drive when it
// is the current one (getcwd, realpath).
std::string LinuxPath(const wchar_t* windows);

// A new CRT fd open on NUL (fd.cc), which reserves a number for an fd that
// gesher-base serves itself: a socket, eventfd or epoll.
int ReserveFd();

// Character devices Windows lacks: a CRT fd on NUL whose reads fd.cc serves.
enum class Device { kZero, kRandom };
// Takes over `fd`, which must be open on NUL.
void RegisterDevice(int fd, Device device);

}  // namespace gesher
