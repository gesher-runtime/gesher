// gesher-base internals shared by the implementation files.
#pragma once

#include <errno.h>
#include <windows.h>

#include <string>
#include <vector>

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

// Microseconds on a monotonic clock (QueryPerformanceCounter). Timeouts keep
// their deadline on it: GetTickCount64 moves in 15.6 ms steps, and a wait
// that ends a step early would return before Linux's would.
ULONGLONG MonotonicMicros();
// A deadline `timeout_ms` from now, or ~0 for a negative (infinite) timeout.
ULONGLONG DeadlineAfter(int timeout_ms);
// Milliseconds left until `deadline`, rounded up; 0 once it has passed.
int MillisUntil(ULONGLONG deadline);

// Where a Linux path leads on Windows (path.cc,
// docs/decisions/0010-paths.md, 0045-posix-paths-tz-procfs.md).
enum class PathKind {
  kFile,     // a file system path, in `windows`
  kNull,     // /dev/null
  kZero,     // /dev/zero
  kRandom,   // /dev/random, /dev/urandom
  kVirtual,  // a name under /proc, or /etc/timezone: procfs.cc, by `windows`
             // (the normalized name, e.g. "proc/self/status", as UTF-16)
  kBinder,   // /dev/binder, when a binderd serves $GESHER_BINDER (binder.cc)
  kMissing,  // any other name under /dev or /sys
};
PathKind MapPath(const char* path, std::wstring* windows);

// A Windows path as Linux code sees it: '/' separators, no drive when it is
// the current one, /mnt/<drive>/... when it is another (getcwd, realpath).
std::string LinuxPath(const wchar_t* windows);

// A Linux path's file on disk, for the calls that take one (stat, open,
// rename, ...): MapPath for kFile, with errno set otherwise: ENOENT for a
// missing or empty name, `special` for a device or virtual file. Returns false
// with errno set when there is no file.
bool DiskPath(const char* path, std::wstring* windows, int special);

// unlink (directory false) and rmdir (true) of a mapped path (path.cc).
int PosixDelete(const std::wstring& windows, bool directory);

// The *at calls' path: one relative to directory fd `dirfd` joined onto its
// path; absolute paths and AT_FDCWD unchanged. False (errno set) if dirfd is
// not a directory.
bool ResolveAt(int dirfd, const char* path, std::string* out);

// Virtual files and directories (procfs.cc), named as MapPath's kVirtual.
namespace procfs {
// S_IFREG, S_IFDIR or S_IFLNK, or 0 when there is no such name.
unsigned Type(const std::wstring& name);
// open(): an fd on a snapshot of the file's content; -1 with errno set.
int Open(const std::wstring& name, int flags);
// opendir(): the directory's entries (name, d_type); false with errno set.
struct Entry {
  std::string name;
  unsigned char type;
};
bool List(const std::wstring& name, std::vector<Entry>* entries);
// readlink(): the link's target as a Linux path; false with errno set.
bool ReadLink(const std::wstring& name, std::string* target);
// n for /proc/self/fd/<n> when fd n is open, else -1.
int FdOfLink(const std::wstring& name);
}  // namespace procfs

// Symbolic links on disk (links.cc): whether a handle opened with
// FILE_FLAG_OPEN_REPARSE_POINT is a symbolic link or junction, and its target
// as a Linux path (relative targets stay relative).
bool IsSymlinkHandle(HANDLE h);
bool ReadSymlinkHandle(HANDLE h, std::string* target);

// The IANA id of Windows' time zone (windows_zones.cc), as /etc/timezone holds
// it; "Etc/UTC" if it has none.
std::string WindowsZoneId();

// A new CRT fd open on NUL (fd.cc), which reserves a number for an fd that
// gesher-base serves itself: a socket, eventfd or epoll.
int ReserveFd();

// Character devices Windows lacks: a CRT fd on NUL whose reads fd.cc serves.
enum class Device { kZero, kRandom };
// Takes over `fd`, which must be open on NUL.
void RegisterDevice(int fd, Device device);

}  // namespace gesher
