// gesher-base: <unistd.h> and <stdlib.h> additions.

#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

#include <atomic>
#include <string>

#include "internal.h"

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
  int mode = _O_BINARY | ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0);
  return gesher::CrtResult(_pipe(fds, 64 * 1024, mode));
}

int pipe(int fds[2]) { return pipe2(fds, 0); }

// Only files are flushed: FlushFileBuffers on a pipe waits until the reader
// has read everything, where Linux fails at once with EINVAL (pipes, sockets,
// character devices).
int fsync(int fd) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (GetFileType(h) != FILE_TYPE_DISK) return errno = EINVAL, -1;
  // A directory (opened for reading, as Linux programs do to fsync a rename):
  // NTFS journals its entries; FlushFileBuffers would need write access.
  BY_HANDLE_FILE_INFORMATION info;
  if (GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return 0;
  }
  return gesher::CrtResult(_commit(fd));
}
int fdatasync(int fd) { return fsync(fd); }

// Note: unlike Linux, these move the file position (Windows has no positioned
// I/O that leaves it alone on synchronous handles).
ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (offset < 0) return errno = EINVAL, -1;
  OVERLAPPED ov = {};
  ov.Offset = static_cast<DWORD>(static_cast<unsigned long long>(offset));
  ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(offset) >> 32);
  DWORD n = 0;
  DWORD want = count > 0x7fffffff ? 0x7fffffff : static_cast<DWORD>(count);
  if (!ReadFile(h, buf, want, &n, &ov)) {
    if (GetLastError() == ERROR_HANDLE_EOF) return 0;
    return gesher::FailWithLastError();
  }
  return static_cast<ssize_t>(n);
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (offset < 0) return errno = EINVAL, -1;
  OVERLAPPED ov = {};
  ov.Offset = static_cast<DWORD>(static_cast<unsigned long long>(offset));
  ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(offset) >> 32);
  DWORD n = 0;
  DWORD want = count > 0x7fffffff ? 0x7fffffff : static_cast<DWORD>(count);
  if (!WriteFile(h, buf, want, &n, &ov)) return gesher::FailWithLastError();
  return static_cast<ssize_t>(n);
}

ssize_t pread64(int fd, void* buf, size_t count, off_t offset) {
  return pread(fd, buf, count, offset);
}
ssize_t pwrite64(int fd, const void* buf, size_t count, off_t offset) {
  return pwrite(fd, buf, count, offset);
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
  if (!overwrite && getenv(name) != nullptr) return 0;
  return _putenv_s(name, value) == 0 ? 0 : (errno = EINVAL, -1);
}

int unsetenv(const char* name) {
  return _putenv_s(name, "") == 0 ? 0 : (errno = EINVAL, -1);
}

}  // extern "C"
