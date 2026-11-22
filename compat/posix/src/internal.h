// gesher-base internals shared by the implementation files.
#pragma once

#include <errno.h>
#include <stdint.h>
#include <windows.h>

#include <string>
#include <memory>
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

// Make invalid CRT descriptors report their documented errors instead of
// terminating the process. Lazy OFD import can run before C++ initializers.
void InitializeCrtDescriptorErrors();

// After a successful write to `fd` (fd.cc's write, pwrite): IN_MODIFY for the
// inotify watches on the file or its directory in this process at once, as
// Linux reports each write; NTFS tells ReadDirectoryChangesW only when it
// updates the directory entry, often at close (inotify.cc, ADR 0057). Cheap
// when no inotify instance exists.
void InotifyNoteWrite(int fd);
// An already pinned file object, for I/O that can outlive close/reuse of fd.
void InotifyNoteWrite(HANDLE file);

// The bottom of the current thread's stack that Windows keeps for itself:
// the lowest page, never committed, and the stack guarantee above it (two
// pages by default on x64, or what SetThreadStackGuarantee set). Touching
// its top page while it is the last guard page raises STATUS_STACK_OVERFLOW.
// pthread_attr_getguardsize reports it, so ART puts its protected region
// right above it (fault_signals.cc, docs/decisions/0036).
size_t StackBottomReserve();

// An access violation at `addr` (access: 0 read, 1 write, 8 execute, as in
// EXCEPTION_RECORD) that only raced madvise(MADV_DONTNEED): mman.cc gives
// private pages back and commits them again, zero-filled, and another
// thread's access between the two faults where Linux would have read zeros.
// Waits until no madvise of that page is in flight; true when the page now
// allows the access, so the faulting instruction can be retried
// (fault_signals.cc).
bool RetryAfterMadvise(uintptr_t addr, ULONG_PTR access);

// Microseconds on the Android monotonic clock (excludes system suspend). Timeouts keep
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
extern "C" const unsigned char* gesher_auxv_random(void);
extern "C" const char* gesher_auxv_platform(void);
extern "C" uint64_t gesher_auxv_hwcap(void);
extern "C" uint64_t gesher_auxv_hwcap2(void);
extern "C" const char* gesher_auxv_execfn(void);

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

// Roll back a private, unpublished duplication while another guarded user
// alias survives. Retires actual virtual resources/bookkeeping without
// treating internal housekeeping as a user POSIX any-close operation.
int CloseUnpublishedDuplicate(int fd);

// Character devices Windows lacks: a CRT fd on NUL whose reads fd.cc serves.
enum class Device { kZero, kRandom };
// Exact logical device behind a NUL-backed fd; false for ordinary NUL handles.
bool DeviceFromFd(int fd, Device* device);
// Takes over `fd`, which must be open on NUL.
void RegisterDevice(int fd, Device device);
// CRT stream close/reopen bypasses close(). Retire its device OFD slot after
// the real operation, while the descriptor guard still prevents fd reuse.
// On failure the CRT may already have closed the descriptor (e.g. after a
// flush error); a still-open descriptor keeps its marker.
void ForgetStreamDevice(int fd, bool closed_or_replaced);

// Open file description management and file status flags (ofd.cc).
int RegisterOfd(int fd, int flags);
int DupOfd(int old_fd, int new_fd);
void ForgetOfd(int fd);
int GetFileStatusFlags(int fd);
int SetFileStatusFlags(int fd, int flags);
bool IsAppendMode(int fd);
bool IsNonBlockingMode(int fd);
// memfd seals (memfd_seals.cc): stored on the file, so every descriptor of it,
// in any process, shares them. Descriptors known to be memfds are registered.
bool InitMemfdSeals(HANDLE file, bool allow_sealing);
void RegisterMemfd(int fd);
void ForgetMemfd(int fd);
bool IsRegisteredMemfd(int fd);
// After receiving a descriptor from another process (Binder): registers it if
// its file carries seals.
void RegisterMemfdIfSealed(int fd, HANDLE file);
// Sockets in Binder transactions (socket.cc; ADR 0088 phase 3). A socket fd
// travels as its SOCKET handle, which binderd duplicates like any other handle,
// with its open description's state as metadata (in the fd object's
// pad_flags, which the kernel ABI leaves unused), since a SOCKET does not
// carry Gesher's state. kTransferredSocket marks such metadata.
inline constexpr uint32_t kTransferredSocket = 0x1;
// False if fd is not a socket; else its handle and description's metadata.
bool TransferSocket(int fd, uintptr_t* socket, uint64_t* metadata);
// A new fd for `socket`, this process's own handle (taken over, closed on
// failure), with the description `metadata` describes; -1 with errno.
int AdoptSocket(uintptr_t socket, uint64_t metadata);
// dup/dup2/F_DUPFD: `to` is a memfd exactly when `from` is.
void CopyMemfdRegistration(int from, int to);
// fcntl F_GET_SEALS / F_ADD_SEALS: -1 with errno (EINVAL for another file).
int GetMemfdSeals(int fd);
int AddMemfdSeals(int fd, unsigned seals);
// 0, or the errno (EPERM) the file's seals give a write of `length` bytes at
// `offset` (or at the descriptor's position), or a resize to `new_size`.
int MemfdWriteError(int fd, bool at_position, uint64_t offset, uint64_t length);
int MemfdResizeError(int fd, uint64_t new_size);
// fallocate up to `end`: EPERM only when it would grow an F_SEAL_GROW memfd.
int MemfdGrowError(int fd, uint64_t end);
// A MAP_SHARED mapping of fd: EPERM when writable and write-sealed; sets
// *deny_write when a write seal must keep the mapping from becoming writable.
int MemfdSharedMapError(int fd, bool writable, bool* deny_write);
// Snapshot I/O must check the pinned object, never a recycled descriptor.
bool FileHasMemfdSeals(HANDLE file);
int MemfdWriteHandleError(HANDLE file, bool at_position, uint64_t offset,
                          uint64_t length, bool append);
int MemfdResizeHandleError(HANDLE file, uint64_t new_size);
int MemfdGrowHandleError(HANDLE file, uint64_t end);
int MemfdSharedMapHandleError(HANDLE file, bool writable, bool* deny_write);

}  // namespace gesher
