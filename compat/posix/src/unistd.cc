// gesher-base: <unistd.h> and <stdlib.h> additions.

#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <gesher/font_environment.h>
#include <gesher/file_description.h>

#include <atomic>
#include <string>

#include "internal.h"
#include "revocable_internal.h"
#include "binder_internal.h"
#include "event_internal.h"
#include "record_lock_descriptors.h"
#include "pipe_io.h"

namespace {

// Android gives each app its own uid; Gesher reports the first app uid, or
// $GESHER_UID when whoever started the process set one (read once, as a
// process's uid does not change).
uid_t AppUid() {
  static const uid_t uid = [] {
    const char* env = getenv("GESHER_UID");
    char* end = nullptr;
    unsigned long value = env != nullptr ? strtoul(env, &end, 10) : 0;
    return env != nullptr && *env != '\0' && *end == '\0' && value < 0x7fffffff
               ? static_cast<uid_t>(value)
               : static_cast<uid_t>(10000);
  }();
  return uid;
}
#define kAppUid AppUid()

std::atomic<unsigned long long> rand_state{0x853c49e6748fea9bULL};

// The file open on a handle, as a Linux path (LinuxPath). False with errno
// set if it has no file name (a pipe, a console, NUL).
bool FinalPath(HANDLE h, std::string* out) {
  wchar_t buf[32768];
  DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0) {
    gesher::FailWithLastError();
    return false;
  }
  if (n >= 32768) {
    errno = ENAMETOOLONG;
    return false;
  }
  *out = gesher::LinuxPath(buf);
  return true;
}

}  // namespace

extern "C" {

long sysconf(int name) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  switch (name) {
    case _SC_PAGESIZE:
      return static_cast<long>(si.dwPageSize);
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
      return static_cast<long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    case _SC_PHYS_PAGES: {
      MEMORYSTATUSEX status{};
      status.dwLength = sizeof(status);
      GlobalMemoryStatusEx(&status);
      return static_cast<long>(status.ullTotalPhys / si.dwPageSize);
    }
    case _SC_AVPHYS_PAGES: {
      MEMORYSTATUSEX status{};
      status.dwLength = sizeof(status);
      GlobalMemoryStatusEx(&status);
      return static_cast<long>(status.ullAvailPhys / si.dwPageSize);
    }
    case _SC_CLK_TCK:
      return 100;
    case _SC_OPEN_MAX:
      return 8192;
    // Limits, as glibc reports them, except ARG_MAX: a Windows command line
    // holds 32767 UTF-16 units.
    case _SC_ARG_MAX:
      return 32767;
    case _SC_CHILD_MAX:
      return -1;  // no limit (errno unchanged)
    case _SC_NGROUPS_MAX:
      return 65536;
    case _SC_STREAM_MAX:
      return 16;
    case _SC_TZNAME_MAX:
      return -1;
    case _SC_LINE_MAX:
      return 2048;
    case _SC_RE_DUP_MAX:
      return 0x7fff;
    case _SC_IOV_MAX:
      return 1024;
    case _SC_GETGR_R_SIZE_MAX:
    case _SC_GETPW_R_SIZE_MAX:
      return 1024;
    case _SC_LOGIN_NAME_MAX:
      return 256;
    case _SC_TTY_NAME_MAX:
      return 32;
    case _SC_HOST_NAME_MAX:
      return 64;
    case _SC_THREAD_DESTRUCTOR_ITERATIONS:
      return 4;
    case _SC_THREAD_KEYS_MAX:
      return 1024;
    case _SC_THREAD_STACK_MIN:
      return 16384;
    case _SC_THREAD_THREADS_MAX:
      return -1;
    case _SC_SEM_VALUE_MAX:
      return 0x7fffffff;
    case _SC_SIGQUEUE_MAX:
    case _SC_RTSIG_MAX:
      return 32;
    // Supported POSIX options report the version they conform to.
    case _SC_VERSION:
    case _SC_THREADS:
    case _SC_THREAD_SAFE_FUNCTIONS:
    case _SC_THREAD_ATTR_STACKADDR:
    case _SC_THREAD_ATTR_STACKSIZE:
    case _SC_MAPPED_FILES:
    case _SC_MEMORY_PROTECTION:
    case _SC_FSYNC:
    case _SC_MONOTONIC_CLOCK:
    case _SC_TIMERS:
      return 200809L;
    case _SC_XOPEN_VERSION:
      return 700;
    case _SC_2_VERSION:
    case _SC_2_C_BIND:
      return 200809L;
    // Options gesher-base does not support.
    case _SC_JOB_CONTROL:
    case _SC_SAVED_IDS:
    case _SC_REALTIME_SIGNALS:
    case _SC_PRIORITY_SCHEDULING:
    case _SC_ASYNCHRONOUS_IO:
    case _SC_PRIORITIZED_IO:
    case _SC_SYNCHRONIZED_IO:
    case _SC_MEMLOCK:
    case _SC_MEMLOCK_RANGE:
    case _SC_MESSAGE_PASSING:
    case _SC_SEMAPHORES:
    case _SC_SHARED_MEMORY_OBJECTS:
    case _SC_THREAD_PRIORITY_SCHEDULING:
    case _SC_THREAD_PRIO_INHERIT:
    case _SC_THREAD_PRIO_PROTECT:
    case _SC_THREAD_PROCESS_SHARED:
    case _SC_XOPEN_UNIX:
    case _SC_XOPEN_CRYPT:
    case _SC_XOPEN_ENH_I18N:
    case _SC_XOPEN_SHM:
    case _SC_XOPEN_LEGACY:
    case _SC_XOPEN_REALTIME:
    case _SC_XOPEN_REALTIME_THREADS:
      return -1;
    default:
      errno = EINVAL;
      return -1;
  }
}

namespace {

long PathconfValue(int name) {
  switch (name) {
    case _PC_LINK_MAX: return 1024;  // NTFS hard links
    case _PC_MAX_CANON:
    case _PC_MAX_INPUT: return 255;
    case _PC_NAME_MAX: return 255;
    case _PC_PATH_MAX: return 4096;
    case _PC_PIPE_BUF: return 4096;
    case _PC_CHOWN_RESTRICTED: return 1;
    case _PC_NO_TRUNC: return 1;
    case _PC_VDISABLE: return 0;
    case _PC_FILESIZEBITS: return 64;
    case _PC_SYMLINK_MAX: return -1;
    case _PC_SYNC_IO:
    case _PC_ASYNC_IO:
    case _PC_PRIO_IO:
    case _PC_REC_INCR_XFER_SIZE:
    case _PC_REC_MAX_XFER_SIZE:
    case _PC_REC_MIN_XFER_SIZE:
    case _PC_REC_XFER_ALIGN:
    case _PC_ALLOC_SIZE_MIN:
    case _PC_2_SYMLINKS:
      return -1;  // no limit or not supported (errno unchanged)
    default:
      errno = EINVAL;
      return -1;
  }
}

// chown as a process without CAP_CHOWN: only "no change" is allowed.
int CheckOwner(uid_t owner, gid_t group) {
  bool same_owner = owner == static_cast<uid_t>(-1) || owner == kAppUid;
  bool same_group = group == static_cast<gid_t>(-1) || group == kAppUid;
  return same_owner && same_group ? 0 : (errno = EPERM, -1);
}

}  // namespace

// The same values for every file system.
long pathconf(const char* path, int name) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return PathconfValue(name);
}

long fpathconf(int fd, int name) {
  if (gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  return PathconfValue(name);
}

int chown(const char* path, uid_t owner, gid_t group) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return CheckOwner(owner, group);
}

int lchown(const char* path, uid_t owner, gid_t group) { return chown(path, owner, group); }

int fchown(int fd, uid_t owner, gid_t group) {
  if (gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  return CheckOwner(owner, group);
}

// The ids cannot change, but setting them to what they are succeeds.
int setuid(uid_t uid) { return uid == kAppUid ? 0 : (errno = EPERM, -1); }
int seteuid(uid_t uid) { return setuid(uid); }
int setgid(gid_t gid) { return gid == kAppUid ? 0 : (errno = EPERM, -1); }
int setegid(gid_t gid) { return setgid(gid); }

int setreuid(uid_t ruid, uid_t euid) {
  bool ok = (ruid == static_cast<uid_t>(-1) || ruid == kAppUid) &&
            (euid == static_cast<uid_t>(-1) || euid == kAppUid);
  return ok ? 0 : (errno = EPERM, -1);
}

int gesher_gethostname(char* name, size_t len) {
  if (name == nullptr) return errno = EFAULT, -1;
  wchar_t host[256];
  DWORD size = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
  if (!GetComputerNameExW(ComputerNameDnsHostname, host, &size)) return gesher::FailWithLastError();
  std::string utf8 = gesher::Narrow(host);
  if (utf8.size() + 1 > len) return errno = ENAMETOOLONG, -1;
  memcpy(name, utf8.c_str(), utf8.size() + 1);
  return 0;
}

int setregid(gid_t rgid, gid_t egid) {
  bool ok = (rgid == static_cast<gid_t>(-1) || rgid == kAppUid) &&
            (egid == static_cast<gid_t>(-1) || egid == kAppUid);
  return ok ? 0 : (errno = EPERM, -1);
}

int getpagesize(void) { return static_cast<int>(sysconf(_SC_PAGESIZE)); }

uid_t getuid(void) { return kAppUid; }
uid_t geteuid(void) { return kAppUid; }
gid_t getgid(void) { return kAppUid; }
gid_t getegid(void) { return kAppUid; }
pid_t gettid(void) { return static_cast<pid_t>(GetCurrentThreadId()); }
pid_t setsid(void) { return static_cast<pid_t>(GetCurrentProcessId()); }

// One process group per process.
int setpgid(pid_t, pid_t) { return 0; }
pid_t getpgid(pid_t) { return static_cast<pid_t>(GetCurrentProcessId()); }
pid_t getpgrp(void) { return static_cast<pid_t>(GetCurrentProcessId()); }

pid_t fork(void) { return errno = ENOSYS, -1; }

int pipe2(int fds[2], int flags) {
  if ((flags & ~(O_CLOEXEC | O_NONBLOCK)) != 0) return errno = EINVAL, -1;
  return gesher::CreatePipeDescriptors(fds, flags);
}

int pipe(int fds[2]) { return pipe2(fds, 0); }

// Only files are flushed: FlushFileBuffers on a pipe waits until the reader
// has read everything, where Linux fails at once with EINVAL (pipes, sockets,
// character devices).
int fsync(int fd) {
  auto snapshot = gesher::CaptureDescriptor(fd);
  if (!snapshot) return -1;
  gesher::FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const HANDLE h = snapshot->handle;
  if (GetFileType(h) != FILE_TYPE_DISK) return errno = EINVAL, -1;
  // A directory (opened for reading, as Linux programs do to fsync a rename):
  // NTFS journals its entries; FlushFileBuffers would need write access.
  BY_HANDLE_FILE_INFORMATION info;
  if (GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return 0;
  }
  return FlushFileBuffers(h) ? 0 : gesher::FailWithLastError();
}
int gesher_ftruncate(int fd, off_t length) {
  if (length < 0) return errno = EINVAL, -1;
  // Pin the exact object without holding the descriptor table lock while a
  // revocation barrier or filesystem operation waits.
  auto snapshot = gesher::CaptureDescriptor(fd);
  if (!snapshot) return -1;
  gesher::FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const HANDLE file = snapshot->handle;
  uint32_t access = 0;
  int error = gesher::record_locks::HandleAccess(file, &access);
  if (error) return errno = error, -1;
  // The CRT can short-circuit an unchanged size. A valid read-only descriptor
  // must still fail ftruncate, including that no-op; keep the guest's EINVAL
  // convention for a descriptor lacking write access.
  if ((access & FILE_WRITE_DATA) == 0) return errno = EINVAL, -1;
  LARGE_INTEGER before{};
  const bool have_size = GetFileSizeEx(file, &before) != FALSE;
  if (!have_size || before.QuadPart != length) {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = length;
    if (!SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof(end)))
      return gesher::FailWithLastError();
  }
  if (!have_size || length == 0 || before.QuadPart != length) return 0;

  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const size_t in_page = static_cast<uint64_t>(length) % system.dwPageSize;
  if (in_page == 0) return 0;

  // Linux also discards cached bytes past EOF when ftruncate leaves the size
  // unchanged. SetEndOfFile/_chsize_s does not. Clear only that part of the
  // shared native final page; neither EOF nor the descriptor cursor moves.
  // Growth must not use this path: Linux preserves cached bytes exposed by a
  // growth within the old final page. Native shrinking already clears them.
  const uint32_t mapping_access = FILE_READ_DATA | FILE_WRITE_DATA;
  if ((access & mapping_access) != mapping_access) {
    // Windows requires both rights for a writable file section. Preserve
    // successful O_WRONLY ftruncate/truncate; never reopen with more rights.
    // Invalidating an existing cached tail through a write-only handle remains
    // a compatibility limit, rather than turning valid resizes into failures.
    return 0;
  }

  HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
  if (!section) return gesher::FailWithLastError();
  const uint64_t offset = static_cast<uint64_t>(length) -
      static_cast<uint64_t>(length) % system.dwAllocationGranularity;
  // A zero view length maps the actual section through EOF, including its
  // existing partial final page. Requesting a rounded section size would grow
  // the file and make these cache-only bytes visible to pread.
  void* view = MapViewOfFile(section, FILE_MAP_WRITE,
                            static_cast<DWORD>(offset >> 32),
                            static_cast<DWORD>(offset), 0);
  DWORD failure = view ? ERROR_SUCCESS : GetLastError();
  if (view) {
    LARGE_INTEGER current{};
    if (!GetFileSizeEx(file, &current)) {
      failure = GetLastError();
    } else if (current.QuadPart == length) {
      // A different length means another operation followed our truncate; do
      // not overwrite its newly valid file data or touch a shorter mapping.
      auto* tail = static_cast<unsigned char*>(view) +
          static_cast<size_t>(static_cast<uint64_t>(length) - offset);
      memset(tail, 0, system.dwPageSize - in_page);
    }
    UnmapViewOfFile(view);
  }
  CloseHandle(section);
  if (failure != ERROR_SUCCESS) return errno = gesher::ErrnoFromWin32(failure), -1;
  return 0;
}

int fdatasync(int fd) { return fsync(fd); }

// Positioned I/O must not change the shared cursor, including through dup().
// ReadFile/WriteFile with OVERLAPPED still advance a synchronous FILE_OBJECT's
// cursor. A duplicate only pins that same object; ReOpenFile creates a separate
// one without resolving a possibly renamed/replaced pathname.
class PositionedHandle {
 public:
  HANDLE value = INVALID_HANDLE_VALUE;
  ~PositionedHandle() {
    if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
  }
};

static int PositionedMode(HANDLE file, DWORD* flags) {
  using Query = LONG(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FILE_INFORMATION_CLASS);
  using Error = ULONG(WINAPI*)(LONG);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return EOPNOTSUPP;
  auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationFile")));
  auto error = reinterpret_cast<Error>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (!query || !error) return EOPNOTSUPP;
  IO_STATUS_BLOCK status{};
  FILE_MODE_INFORMATION mode{};
  LONG result = query(file, &status, &mode, sizeof(mode), FileModeInformation);
  if (result < 0) return gesher::ErrnoFromWin32(error(result));
  // The new private object is asynchronous. Keep actual data-integrity and
  // alignment modes; duplicating DELETE_ON_CLOSE would change deletion lifetime.
  *flags = FILE_FLAG_OVERLAPPED |
      ((mode.Mode & FILE_WRITE_THROUGH) ? FILE_FLAG_WRITE_THROUGH : 0) |
      ((mode.Mode & FILE_NO_INTERMEDIATE_BUFFERING) ? FILE_FLAG_NO_BUFFERING : 0) |
      ((mode.Mode & FILE_SEQUENTIAL_ONLY) ? FILE_FLAG_SEQUENTIAL_SCAN : 0);
  return 0;
}

static int PositionedNullDevice(HANDLE file, bool* is_null) {
  using Query = LONG(WINAPI*)(HANDLE, PIO_STATUS_BLOCK, void*, ULONG, FS_INFORMATION_CLASS);
  using Error = ULONG(WINAPI*)(LONG);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return EOPNOTSUPP;
  auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryVolumeInformationFile")));
  auto error = reinterpret_cast<Error>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (!query || !error) return EOPNOTSUPP;
  IO_STATUS_BLOCK status{};
  FILE_FS_DEVICE_INFORMATION device{};
  LONG result = query(file, &status, &device, sizeof(device), FileFsDeviceInformation);
  if (result < 0) return gesher::ErrnoFromWin32(error(result));
  *is_null = device.DeviceType == FILE_DEVICE_NULL;
  return 0;
}

static ssize_t PositionedNativeIo(HANDLE anchor, DWORD type, DWORD access, bool append,
                                   void* buf, size_t count, off_t offset, bool writing) {
  PositionedHandle positioned, event;
  HANDLE file = anchor;
  const DWORD want = count > 0x7ffff000 ? 0x7ffff000 : static_cast<DWORD>(count);
  if (type == FILE_TYPE_DISK) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file, &information)) return gesher::FailWithLastError();
    if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return errno = EISDIR, -1;
    if (want == 0) return 0;
    if (buf == nullptr) {
      // An EOF read consumes no destination memory. Observe the real pinned
      // file's length, without seeking or resolving its pathname.
      if (!writing) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) return gesher::FailWithLastError();
        if (offset >= size.QuadPart) return 0;
      }
      return errno = EFAULT, -1;
    }
    DWORD flags;
    int error = PositionedMode(file, &flags);
    if (error) return errno = error, -1;
    positioned.value = ReOpenFile(file, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, flags);
    if (positioned.value == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
    file = positioned.value;
  }
  if (count != 0 && buf == nullptr) {
    // Only a proven NUL device may discard an unused buffer. A console,
    // socket, or other character driver is never classified by type alone.
    if (type == FILE_TYPE_CHAR) {
      bool is_null = false;
      int error = PositionedNullDevice(file, &is_null);
      if (error) return errno = error, -1;
      if (is_null) return writing ? static_cast<ssize_t>(want) : 0;
    }
    return errno = EFAULT, -1;
  }
  event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event.value == nullptr) return gesher::FailWithLastError();
  OVERLAPPED ov{};
  ov.hEvent = event.value;
  ov.Offset = append ? MAXDWORD : static_cast<DWORD>(static_cast<unsigned long long>(offset));
  ov.OffsetHigh = append ? MAXDWORD : static_cast<DWORD>(static_cast<unsigned long long>(offset) >> 32);
  DWORD transferred = 0;
  BOOL done = writing ? WriteFile(file, buf, want, &transferred, &ov)
                      : ReadFile(file, buf, want, &transferred, &ov);
  DWORD error = done ? ERROR_SUCCESS : GetLastError();
  if (!done && error == ERROR_IO_PENDING) {
    // Keep the buffer, OVERLAPPED, event and both file objects alive until the
    // operation has completed. The caller sees synchronous POSIX semantics.
    done = GetOverlappedResult(file, &ov, &transferred, TRUE);
    error = done ? ERROR_SUCCESS : GetLastError();
  }
  if (!done) {
    if (!writing && error == ERROR_HANDLE_EOF) return 0;
    if (error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER) return errno = EFAULT, -1;
    return errno = gesher::ErrnoFromWin32(error), -1;
  }
  if (writing && transferred > 0) gesher::InotifyNoteWrite(anchor);
  return static_cast<ssize_t>(transferred);
}

static ssize_t PositionedIo(int fd, void* buf, size_t count, off_t offset, bool writing) {
  if (offset < 0) return errno = EINVAL, -1;
  gesher::DescriptorSnapshotPtr snapshot;
  gesher::Device device{};
  bool is_device = false;
  // Linux's MAX_RW_COUNT for the 4 KiB pages exposed by this Windows backend.
  const DWORD want = count > 0x7ffff000 ? 0x7ffff000 : static_cast<DWORD>(count);
  {
    // Capture the exact object before close/dup2 can recycle its fd or HANDLE.
    // No descriptor lock is held during the filesystem open or pending I/O.
    gesher::record_locks::DescriptorGuard descriptor_guard;
    HANDLE file = gesher::HandleFromFd(fd);
    if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
    short ready;
    if (gesher_is_socket(fd) || gesher::IsBinderFd(fd) || gesher::LookupEventFd(fd) ||
        gesher::PollEpoll(fd, 0, &ready)) return errno = ESPIPE, -1;
    snapshot = gesher::CaptureDescriptor(fd);
    is_device = gesher::DeviceFromFd(fd, &device);
  }
  if (!snapshot) return -1;
  gesher::FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const HANDLE file = snapshot->handle;
  SetLastError(ERROR_SUCCESS);
  const DWORD type = GetFileType(file);
  if (type == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS) return gesher::FailWithLastError();
  if (type == FILE_TYPE_PIPE) return errno = ESPIPE, -1;
  DWORD console_mode;
  if (type == FILE_TYPE_CHAR && GetConsoleMode(file, &console_mode)) return errno = ESPIPE, -1;
  const int status_flags = gesher::FileDescriptionFlags(snapshot);
  if (status_flags < 0) return -1;
  if ((status_flags & O_PATH) != 0 ||
      (status_flags & O_ACCMODE) == (writing ? O_RDONLY : O_WRONLY)) return errno = EBADF, -1;
  // Android uses Linux's pwrite behavior: O_APPEND overrides the explicit
  // offset, but the caller's shared ordinary file position stays unchanged.
  const bool append = writing && type == FILE_TYPE_DISK && (status_flags & O_APPEND) != 0;
  uint32_t granted = 0;
  int error = gesher::record_locks::HandleAccess(file, &granted);
  if (error) return errno = error, -1;
  const DWORD access = writing ? (granted & FILE_WRITE_DATA ? FILE_WRITE_DATA : FILE_APPEND_DATA) : FILE_READ_DATA;
  // ReOpenFile can request more access than the original. Never use that to
  // turn O_RDONLY/O_WRONLY/O_PATH descriptors into writable/readable ones.
  if ((granted & access) == 0) return errno = EBADF, -1;
  if (count != 0 && buf == nullptr) {
    // Linux access_ok checks the user address range before dispatch, even
    // when a backend will not actually dereference the buffer.
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (count - 1 > reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress))
      return errno = EFAULT, -1;
  }
  if (is_device) {
    if (!writing) {
      // Zero/random reads really fill the caller's memory. Unlike Windows
      // ReadFile, the virtual device backend does not probe bad pointers.
      if (count != 0 && buf == nullptr) return errno = EFAULT, -1;
      return gesher::ReadFileDescription(snapshot, buf, want);
    }
    // Random writes consume entropy input on Linux, whereas zero writes
    // discard it. The two logical devices share the same native NUL handle.
    if (device == gesher::Device::kRandom && count != 0 && buf == nullptr)
      return errno = EFAULT, -1;
  }
  return PositionedNativeIo(file, type, access, append, buf, count, offset, writing);
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  return PositionedIo(fd, buf, count, offset, false);
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
  return PositionedIo(fd, const_cast<void*>(buf), count, offset, true);
}

ssize_t pread64(int fd, void* buf, size_t count, off_t offset) {
  return pread(fd, buf, count, offset);
}
ssize_t pwrite64(int fd, const void* buf, size_t count, off_t offset) {
  return pwrite(fd, buf, count, offset);
}

// MinGW declares lseek as the lseek64 symbol under _FILE_OFFSET_BITS=64.
// Every host caller and the bionic bridge therefore use this same policy.
off_t lseek(int fd, off_t offset, int whence) {
  gesher::DescriptorSnapshotPtr snapshot;
  bool is_stream = false, no_position = false;
  {
    gesher::record_locks::DescriptorGuard descriptor_guard;
    HANDLE file = gesher::HandleFromFd(fd);
    if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
    snapshot = gesher::CaptureDescriptor(fd);
    is_stream = gesher_is_socket(fd) || gesher::IsBinderFd(fd);
    short ready;
    // Linux anon-inode event descriptors and these memory devices use
    // noop_llseek: they have no byte position and return zero even for a
    // negative offset. A NUL placeholder alone is not evidence of this kind.
    gesher::Device device;
    no_position = gesher::LookupEventFd(fd) || gesher::PollEpoll(fd, 0, &ready) ||
                  gesher::DeviceFromFd(fd, &device);
  }
  if (!snapshot) return -1;
  gesher::FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const int flags = gesher::FileDescriptionFlags(snapshot);
  if (flags < 0) return -1;
  if ((flags & O_PATH) != 0) return errno = EBADF, -1;
  if (whence < SEEK_SET || whence > SEEK_HOLE) return errno = EINVAL, -1;
  if (is_stream) return errno = ESPIPE, -1;
  if (no_position) return 0;
  const HANDLE file = snapshot->handle;
  SetLastError(ERROR_SUCCESS);
  DWORD type = GetFileType(file);
  if (type == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS) return gesher::FailWithLastError();
  if (type == FILE_TYPE_PIPE) return errno = ESPIPE, -1;
  if (type == FILE_TYPE_CHAR) {
    DWORD console_mode;
    if (GetConsoleMode(file, &console_mode)) return errno = ESPIPE, -1;
    bool is_null = false;
    int error = PositionedNullDevice(file, &is_null);
    if (error) return errno = error, -1;
    return is_null ? 0 : (errno = ESPIPE, -1);
  }
  DWORD origin = static_cast<DWORD>(whence);
  LARGE_INTEGER distance{}, result{};
  distance.QuadPart = offset;
  if (whence == SEEK_DATA || whence == SEEK_HOLE) {
    // Linux's generic filesystem fallback may treat every byte before EOF as
    // data. This backend does not claim finer sparse-extent information.
    if (offset < 0) return errno = EINVAL, -1;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) return gesher::FailWithLastError();
    if (offset >= size.QuadPart) return errno = ENXIO, -1;
    if (whence == SEEK_HOLE) distance = size;
    origin = FILE_BEGIN;
  }
  if (!SetFilePointerEx(file, distance, &result, origin)) return gesher::FailWithLastError();
  return static_cast<off_t>(result.QuadPart);
}

int nice(int inc) {
  (void)inc;
  return 0;
}

// --- <stdlib.h> --------------------------------------------------------------

int gesher_rand(void) {
  // xorshift64*, top 31 bits.
  unsigned long long x = rand_state.load(std::memory_order_relaxed);
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rand_state.store(x, std::memory_order_relaxed);
  return static_cast<int>((x * 0x2545F4914F6CDD1DULL) >> 33);
}

void gesher_srand(unsigned int seed) {
  rand_state.store(0x9E3779B97F4A7C15ULL ^ seed, std::memory_order_relaxed);
}

long random(void) { return gesher_rand(); }

void srandom(unsigned int seed) { gesher_srand(seed); }

// The result is a Linux path (path.cc): without the drive when it is the
// current one, /mnt/<drive>/... when it is another; symbolic links resolved,
// /proc links too (/proc/self/fd/<n>, /proc/self/exe); other names under
// /dev and /proc as they are.
char* realpath(const char* path, char* resolved_path) {
  if (path == nullptr) {
    errno = EINVAL;
    return nullptr;
  }
  std::string out;
  std::string current = path;
  std::wstring wpath;
  gesher::PathKind kind = path[0] != '\0' ? gesher::MapPath(path, &wpath) : gesher::PathKind::kMissing;
  for (int depth = 0; kind == gesher::PathKind::kVirtual; ++depth) {
    unsigned type = gesher::procfs::Type(wpath);
    if (type == 0) {
      errno = ENOENT;
      return nullptr;
    }
    if (type != S_IFLNK) {
      out = "/" + gesher::Narrow(wpath.c_str());
      break;
    }
    std::string target;
    if (!gesher::procfs::ReadLink(wpath, &target)) return nullptr;
    if (target.empty() || target[0] != '/' || depth > 8) {
      out = target;  // pipe:[n], socket:[n]: not a path
      break;
    }
    kind = gesher::MapPath(target.c_str(), &wpath);
    current = target;
    out = target;
  }
  if (kind == gesher::PathKind::kMissing) {
    errno = ENOENT;
    return nullptr;
  }
  if (kind == gesher::PathKind::kFile) {
    HANDLE h = CreateFileW(wpath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      gesher::FailWithLastError();
      return nullptr;
    }
    bool ok = FinalPath(h, &out);
    CloseHandle(h);
    if (!ok) return nullptr;
  } else if (kind != gesher::PathKind::kVirtual) {
    // A device: the name as given, repeated slashes folded.
    out.clear();
    for (const char* p = current.c_str(); *p != '\0'; ++p) {
      if (*p != '/' || out.empty() || out.back() != '/') out += *p;
    }
  }
  if (resolved_path == nullptr) return _strdup(out.c_str());
  if (out.size() >= 4096) {
    errno = ENAMETOOLONG;
    return nullptr;
  }
  memcpy(resolved_path, out.c_str(), out.size() + 1);
  return resolved_path;
}

int setenv(const char* name, const char* value, int overwrite) {
  if (auto result = gesher::WriteFontEnvironment(name, value, overwrite))
    return *result == 0 ? 0 : (errno = EINVAL, -1);
  if (!overwrite && getenv(name) != nullptr) return 0;
  return _putenv_s(name, value) == 0 ? 0 : (errno = EINVAL, -1);
}

int unsetenv(const char* name) {
  if (auto result = gesher::WriteFontEnvironment(name, "", 1))
    return *result == 0 ? 0 : (errno = EINVAL, -1);
  return _putenv_s(name, "") == 0 ? 0 : (errno = EINVAL, -1);
}

}  // extern "C"

namespace gesher {
int64_t WriteFileDescriptionAt(const DescriptorSnapshotPtr& snapshot, const void* bytes,
                              unsigned count, int64_t offset) {
  if (offset < 0) return errno = EINVAL, -1;
  FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const int flags = FileDescriptionFlags(snapshot);
  if (flags < 0) return -1;
  if ((flags & O_PATH) != 0 || (flags & O_ACCMODE) == O_RDONLY) return errno = EBADF, -1;
  if (snapshot->kind != DescriptorKind::kFile || GetFileType(snapshot->handle) != FILE_TYPE_DISK)
    return errno = ESPIPE, -1;
  uint32_t granted = 0;
  if (int error = record_locks::HandleAccess(snapshot->handle, &granted)) return errno = error, -1;
  const DWORD access = granted & FILE_WRITE_DATA ? FILE_WRITE_DATA : FILE_APPEND_DATA;
  if ((granted & access) == 0) return errno = EBADF, -1;
  return PositionedNativeIo(snapshot->handle, FILE_TYPE_DISK, access, (flags & O_APPEND) != 0,
                            const_cast<void*>(bytes), count, offset, true);
}
}  // namespace gesher
