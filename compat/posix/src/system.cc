// gesher-base: smaller POSIX/Linux interfaces — uname, statvfs, resource
// limits and priorities, scheduling, ioctl, wait, fcntl, uio, extra
// stat calls, memfd, strsignal, and link.h.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <link.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <windows.h>
#include <psapi.h>

#include <string>
#include <vector>

#include "binder_internal.h"
#include "event_internal.h"
#include "internal.h"
#include "linux_socket.h"
#include "record_lock_descriptors.h"
#include "record_lock_duplicate.h"
#include "socket_internal.h"

namespace {

HANDLE OpenThreadForPriority(id_t who, DWORD access) {
  if (who == 0 || who == GetCurrentThreadId()) return GetCurrentThread();
  return OpenThread(access, FALSE, who);
}

void CloseThreadHandle(HANDLE h) {
  if (h != GetCurrentThread()) CloseHandle(h);
}

// Linux nice values (-20 highest .. 19 lowest) onto Windows thread priorities.
int PriorityFromNice(int nice) {
  if (nice <= -16) return THREAD_PRIORITY_HIGHEST;
  if (nice <= -6) return THREAD_PRIORITY_ABOVE_NORMAL;
  if (nice <= 4) return THREAD_PRIORITY_NORMAL;
  if (nice <= 14) return THREAD_PRIORITY_BELOW_NORMAL;
  return THREAD_PRIORITY_LOWEST;
}

int NiceFromPriority(int priority) {
  if (priority >= THREAD_PRIORITY_HIGHEST) return -20;
  if (priority >= THREAD_PRIORITY_ABOVE_NORMAL) return -10;
  if (priority >= THREAD_PRIORITY_NORMAL) return 0;
  if (priority >= THREAD_PRIORITY_BELOW_NORMAL) return 10;
  return 19;
}

void FillStatvfs(ULARGE_INTEGER avail, ULARGE_INTEGER total, ULARGE_INTEGER free,
                 struct statvfs* buf) {
  const unsigned long long block = 4096;
  memset(buf, 0, sizeof(*buf));
  buf->f_bsize = block;
  buf->f_frsize = block;
  buf->f_blocks = total.QuadPart / block;
  buf->f_bfree = free.QuadPart / block;
  buf->f_bavail = avail.QuadPart / block;
  buf->f_namemax = 255;
}

}  // namespace

extern "C" {

// --- uname -------------------------------------------------------------------

int uname(struct utsname* buf) {
  memset(buf, 0, sizeof(*buf));
  // Not "Linux": kernel-version checks must not pass.
  snprintf(buf->sysname, sizeof(buf->sysname), "Windows");
  snprintf(buf->release, sizeof(buf->release), "0.0.0-gesher");
  OSVERSIONINFOEXW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  if (rtl_get_version != nullptr && rtl_get_version(&v) == 0) {
    snprintf(buf->version, sizeof(buf->version), "Windows %lu.%lu.%lu", v.dwMajorVersion,
             v.dwMinorVersion, v.dwBuildNumber);
  }
  snprintf(buf->machine, sizeof(buf->machine), "x86_64");
  DWORD size = sizeof(buf->nodename);
  GetComputerNameA(buf->nodename, &size);
  return 0;
}

// --- statvfs -----------------------------------------------------------------

// Android's own directories (`/data`: SQLiteGlobal's default page size is
// StatFs("/data")'s block size; `/system`) are not on the disk (ADR 0010): an
// absolute path that does not exist answers for the volume it would be on, its
// nearest existing ancestor's.
int statvfs(const char* path, struct statvfs* buf) {
  ULARGE_INTEGER avail, total, free;
  std::wstring w = gesher::Widen(path);
  while (!GetDiskFreeSpaceExW(w.c_str(), &avail, &total, &free)) {
    DWORD error = GetLastError();
    size_t slash = w.find_last_of(L"/\\");
    bool missing = error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND;
    if (!missing || (path[0] != '/' && path[0] != '\\') || slash == std::wstring::npos ||
        w.size() <= 1) {
      SetLastError(error);
      return gesher::FailWithLastError();
    }
    w.resize(slash == 0 ? 1 : slash);
  }
  FillStatvfs(avail, total, free, buf);
  return 0;
}

int fstatvfs(int fd, struct statvfs* buf) {
  wchar_t path[MAX_PATH];
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE || GetFinalPathNameByHandleW(h, path, MAX_PATH, 0) == 0) {
    return errno = EBADF, -1;
  }
  ULARGE_INTEGER avail, total, free;
  if (!GetDiskFreeSpaceExW(path, &avail, &total, &free)) return gesher::FailWithLastError();
  FillStatvfs(avail, total, free, buf);
  return 0;
}

// --- mount -------------------------------------------------------------------

int mount(const char*, const char*, const char*, unsigned long, const void*) {
  return errno = ENOSYS, -1;
}
int umount2(const char*, int) { return errno = ENOSYS, -1; }

// --- resources and priorities -------------------------------------------------

// getrlimit, setrlimit and getrusage are in process.cc.

int getpriority(int which, id_t who) {
  if (which != PRIO_PROCESS) return errno = EINVAL, -1;
  HANDLE h = OpenThreadForPriority(who, THREAD_QUERY_LIMITED_INFORMATION);
  if (h == nullptr) return errno = ESRCH, -1;
  int priority = GetThreadPriority(h);
  CloseThreadHandle(h);
  errno = 0;
  return NiceFromPriority(priority);
}

int setpriority(int which, id_t who, int prio) {
  if (which != PRIO_PROCESS) return errno = EINVAL, -1;
  HANDLE h = OpenThreadForPriority(who, THREAD_SET_LIMITED_INFORMATION);
  if (h == nullptr) return errno = ESRCH, -1;
  BOOL ok = SetThreadPriority(h, PriorityFromNice(prio));
  CloseThreadHandle(h);
  return ok ? 0 : gesher::FailWithLastError();
}

// --- scheduling ----------------------------------------------------------------

int gesher_cpu_count(const cpu_set_t* set) {
  int n = 0;
  for (unsigned long long word : set->__bits) n += __builtin_popcountll(word);
  return n;
}

int sched_getcpu(void) { return static_cast<int>(GetCurrentProcessorNumber()); }

int sched_setaffinity(pid_t, size_t, const cpu_set_t*) { return 0; }

int sched_getaffinity(pid_t, size_t size, cpu_set_t* set) {
  memset(set, 0, size);
  DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  for (DWORD i = 0; i < cpus && i < CPU_SETSIZE; ++i) CPU_SET(i, set);
  return 0;
}

int unshare(int) { return errno = ENOSYS, -1; }

int sched_getparam(pid_t, struct sched_param* param) {
  param->sched_priority = 0;
  return 0;
}

int sched_setparam(pid_t, const struct sched_param*) { return 0; }

// --- ioctl, wait --------------------------------------------------------------

// FIONREAD and FIONBIO, the ioctls Android's Java libraries use on files,
// pipes and sockets; the only device ioctls are /dev/binder's (binder.cc),
// others fail with ENOTTY.
int ioctl(int fd, unsigned long request, ...) {
  va_list args;
  va_start(args, request);
  int* argp = va_arg(args, int*);
  va_end(args);
  int binder_result;
  if (gesher::BinderIoctl(fd, request, argp, &binder_result)) return binder_result;
  gesher::SocketFd state;
  if (gesher::LookupSocket(fd, &state)) {
    if (request == FIONREAD) return gesher::ws::Available(state.socket, argp);
    int result;
    if (gesher::sock::InterfaceIoctl(request, argp, &result)) return result;
    if (request == FIONBIO) {
      gesher::record_locks::DescriptorGuard descriptor_guard;
      gesher::SocketFd current;
      if (!gesher::LookupSocket(fd, &current) || current.description != state.description) return errno = EBADF, -1;
      std::lock_guard<std::mutex> lock(state.description->status_mutex);
      const bool on = *argp != 0;
      if (gesher::ws::SetNonBlocking(state.socket, on) < 0) return -1;
      state.description->nonblocking.store(on);
      return 0;
    }
    return errno = ENOTTY, -1;
  }
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (request != FIONREAD) return errno = ENOTTY, -1;
  switch (GetFileType(h)) {
    case FILE_TYPE_PIPE: {
      DWORD avail = 0;
      if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) avail = 0;  // writer gone
      *argp = static_cast<int>(avail > 0x7FFFFFFF ? 0x7FFFFFFF : avail);
      return 0;
    }
    case FILE_TYPE_DISK: {
      // Bytes from the current position to the end, as Linux reports.
      LARGE_INTEGER size, pos, zero = {};
      if (!GetFileSizeEx(h, &size) || !SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) {
        return gesher::FailWithLastError();
      }
      long long remaining = size.QuadPart > pos.QuadPart ? size.QuadPart - pos.QuadPart : 0;
      *argp = static_cast<int>(remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining);
      return 0;
    }
    default:
      *argp = 0;
      return 0;
  }
}

pid_t waitpid(pid_t, int*, int) { return errno = ECHILD, -1; }
pid_t wait(int*) { return errno = ECHILD, -1; }
int waitid(idtype_t, id_t, siginfo_t*, int) { return errno = ECHILD, -1; }

// --- fcntl and friends --------------------------------------------------------

int fcntl(int fd, int cmd, ...) {
  static_assert(sizeof(struct flock) == sizeof(gesher::record_locks::LinuxFlock));
  static_assert(offsetof(struct flock, l_start) == 8 && offsetof(struct flock, l_pid) == 24);
  // Win64 long is32bits. Consume Linux lock pointers BEFORE entering the
  // unchanged integer-command varargs path.
  if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_OFD_GETLK || cmd == F_OFD_SETLK) {
    va_list pointers;
    va_start(pointers, cmd);
    struct flock* argument = va_arg(pointers, struct flock*);
    va_end(pointers);
    return gesher::record_locks::DescriptorFcntl(fd, cmd, argument);
  }
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  va_list args;
  va_start(args, cmd);
  long arg = va_arg(args, long);
  va_end(args);
  gesher::SocketFd state;
  if (gesher::LookupSocket(fd, &state)) {
    switch (cmd) {
      case F_GETFL:
        return O_RDWR | (state.description->nonblocking.load() ? O_NONBLOCK : 0);
      case F_SETFL:
        {
          gesher::record_locks::DescriptorGuard descriptor_guard;
          gesher::SocketFd current;
          if (!gesher::LookupSocket(fd, &current) || current.description != state.description) return errno = EBADF, -1;
          std::lock_guard<std::mutex> lock(state.description->status_mutex);
          const bool on = (arg & O_NONBLOCK) != 0;
          if (on != state.description->nonblocking.load()) {
            if (gesher::ws::SetNonBlocking(state.socket, on) < 0) return -1;
            state.description->nonblocking.store(on);
          }
        }
        return 0;
      case F_GETFD:
        return FD_CLOEXEC;  // sockets are never inherited (winsock.cc)
      case F_SETFD:
        return 0;
      default:
        break;  // F_DUPFD and the rest below
    }
  }
  if (std::shared_ptr<gesher::EventFd> e = gesher::LookupEventFd(fd)) {
    if (cmd == F_GETFL) return O_RDWR | (gesher::IsNonBlocking(*e) ? O_NONBLOCK : 0);
    if (cmd == F_SETFL) {
      gesher::SetNonBlocking(*e, (arg & O_NONBLOCK) != 0);
      return 0;
    }
  }
  switch (cmd) {
    case F_GETFD: {
      DWORD flags = 0;
      if (!GetHandleInformation(h, &flags)) return gesher::FailWithLastError();
      return (flags & HANDLE_FLAG_INHERIT) != 0 ? 0 : FD_CLOEXEC;
    }
    case F_SETFD:
      if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT,
                                (arg & FD_CLOEXEC) != 0 ? 0 : HANDLE_FLAG_INHERIT)) {
        return gesher::FailWithLastError();
      }
      return 0;
    case F_GETFL:
      return O_RDWR;
    case F_SETFL:
      return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
      return gesher::record_locks::DuplicateAtLeast(fd, static_cast<int>(arg), cmd == F_DUPFD_CLOEXEC);
    }
    case F_ADD_SEALS:
    case F_GET_SEALS:
      return 0;  // seals are advisory here
    default:
      return errno = EINVAL, -1;
  }
}

// flock: a lock on one byte far beyond any file's data (LockFileEx locks
// are mandatory on Windows, so a range over the data would block reads and
// writes through other handles; this one blocks nothing but other locks).
// Windows keeps a lock per open file object, as Linux keeps flock's per open
// file description: dup()s share it, and it goes when the last handle closes.
// A lock is converted by dropping it first, as Linux may.
int flock(int fd, int operation) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  int kind = operation & ~LOCK_NB;
  if (kind != LOCK_SH && kind != LOCK_EX && kind != LOCK_UN) return errno = EINVAL, -1;
  if (GetFileType(h) != FILE_TYPE_DISK) return 0;  // pipes, sockets, devices
  OVERLAPPED ov = {};
  ov.Offset = 0xfffffffe;
  ov.OffsetHigh = 0x7fffffff;
  // Drop what this open file holds (none: ERROR_NOT_LOCKED).
  UnlockFileEx(h, 0, 1, 0, &ov);
  if (kind == LOCK_UN) return 0;
  DWORD flags = (kind == LOCK_EX ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                ((operation & LOCK_NB) != 0 ? LOCKFILE_FAIL_IMMEDIATELY : 0);
  memset(&ov, 0, sizeof(ov));
  ov.Offset = 0xfffffffe;
  ov.OffsetHigh = 0x7fffffff;
  if (!LockFileEx(h, flags, 0, 1, 0, &ov)) {
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {  // an overlapped handle: wait for the lock
      DWORD n;
      if (GetOverlappedResult(h, &ov, &n, TRUE)) return 0;
      error = GetLastError();
    }
    if (error == ERROR_LOCK_VIOLATION) return errno = EWOULDBLOCK, -1;
    SetLastError(error);
    return gesher::FailWithLastError();
  }
  return 0;
}

int posix_fadvise(int, off_t, off_t, int) { return 0; }

int posix_fallocate(int fd, off_t offset, off_t len) {
  long long size = _filelengthi64(fd);
  if (size < 0) return EBADF;
  if (offset + len > size) {
    int result = _chsize_s(fd, offset + len);
    if (result != 0) return gesher_errno_from_crt(result);
  }
  return 0;
}

int fallocate(int fd, int mode, off_t offset, off_t len) {
  if (mode != 0) return errno = EOPNOTSUPP, -1;
  int result = posix_fallocate(fd, offset, len);
  return result == 0 ? 0 : (errno = result, -1);
}

// --- uio ---------------------------------------------------------------------

namespace {

// readv/writev on a socket fd: one gather/scatter call.
bool SocketIo(int fd, const struct iovec* iov, int iovcnt, bool write, ssize_t* result) {
  gesher::SocketFd state;
  if (!gesher::LookupSocket(fd, &state)) return false;
  if (iovcnt < 0) {
    errno = EINVAL;
    *result = -1;
    return true;
  }
  std::vector<gesher::ws::Buffer> buffers;
  for (int i = 0; i < iovcnt; ++i) buffers.push_back({iov[i].iov_base, iov[i].iov_len});
  *result = write ? gesher::ws::Send(state.socket, buffers.data(), buffers.size(), 0, nullptr)
                  : gesher::ws::Recv(state.socket, buffers.data(), buffers.size(), 0, nullptr);
  return true;
}

// Linux checks the fd and the count before transferring anything, so a bad
// fd fails even with no buffers.
bool BadVector(int fd, int iovcnt) {
  if (gesher::HandleFromFd(fd) == INVALID_HANDLE_VALUE) return errno = EBADF, true;
  if (iovcnt < 0 || iovcnt > IOV_MAX) return errno = EINVAL, true;
  return false;
}

}  // namespace

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
  if (BadVector(fd, iovcnt)) return -1;
  ssize_t total = 0;
  if (SocketIo(fd, iov, iovcnt, false, &total)) return total;
  for (int i = 0; i < iovcnt; ++i) {
    int n = read(fd, iov[i].iov_base, static_cast<unsigned>(iov[i].iov_len));  // fd.cc
    if (n < 0) return total > 0 ? total : -1;
    total += n;
    if (static_cast<size_t>(n) < iov[i].iov_len) break;
  }
  return total;
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
  if (BadVector(fd, iovcnt)) return -1;
  ssize_t total = 0;
  if (SocketIo(fd, iov, iovcnt, true, &total)) return total;
  for (int i = 0; i < iovcnt; ++i) {
    int n = gesher::CrtResult(_write(fd, iov[i].iov_base, static_cast<unsigned>(iov[i].iov_len)));
    if (n < 0) return total > 0 ? total : -1;
    total += n;
    if (static_cast<size_t>(n) < iov[i].iov_len) break;
  }
  return total;
}

ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  if (BadVector(fd, iovcnt)) return -1;
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    ssize_t n = pread(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
    if (n < 0) return total > 0 ? total : -1;
    total += n;
    if (static_cast<size_t>(n) < iov[i].iov_len) break;
  }
  return total;
}

ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset) {
  if (BadVector(fd, iovcnt)) return -1;
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; ++i) {
    ssize_t n = pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset + total);
    if (n < 0) return total > 0 ? total : -1;
    total += n;
    if (static_cast<size_t>(n) < iov[i].iov_len) break;
  }
  return total;
}

ssize_t process_vm_readv(pid_t, const struct iovec*, unsigned long, const struct iovec*,
                         unsigned long, unsigned long) {
  return errno = ENOSYS, -1;
}

// --- extra stat calls (lstat, fstatat, mkdirat: path.cc) ----------------------

// fchown is in unistd.cc, with chown.
int mkfifo(const char*, mode_t) { return errno = ENOSYS, -1; }

int mknod(const char* path, mode_t mode, dev_t) {
  switch (mode & S_IFMT) {
    case 0:
    case S_IFREG: {
      int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, mode & 07777);
      return fd < 0 ? -1 : close(fd);
    }
    case S_IFIFO:
      return mkfifo(path, mode);
    default:
      return errno = EPERM, -1;  // device nodes need CAP_MKNOD
  }
}

ssize_t splice(int, off_t*, int, off_t*, size_t, unsigned int) { return errno = EINVAL, -1; }


// --- memfd -------------------------------------------------------------------

int memfd_create(const char* name, unsigned int flags) {
  (void)name;
  wchar_t dir[MAX_PATH], path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, dir) == 0 || GetTempFileNameW(dir, L"mfd", 0, path) == 0) {
    return gesher::FailWithLastError();
  }
  HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  gesher::record_locks::DescriptorGuard descriptor_guard;
  int fd = gesher::CrtResult(_open_osfhandle(reinterpret_cast<intptr_t>(h),
                           _O_RDWR | _O_BINARY | ((flags & MFD_CLOEXEC) != 0 ? _O_NOINHERIT : 0)));
  if (fd < 0) CloseHandle(h);
  return fd;
}

// --- strsignal, link.h ------------------------------------------------------------

char* strsignal(int sig) {
  static thread_local char buf[32];
  switch (sig) {
    case SIGSEGV: return const_cast<char*>("Segmentation fault");
    case SIGBUS: return const_cast<char*>("Bus error");
    case SIGILL: return const_cast<char*>("Illegal instruction");
    case SIGFPE: return const_cast<char*>("Floating point exception");
    case SIGABRT: return const_cast<char*>("Aborted");
    case SIGTRAP: return const_cast<char*>("Trace/breakpoint trap");
    case SIGQUIT: return const_cast<char*>("Quit");
    case SIGKILL: return const_cast<char*>("Killed");
    case SIGTERM: return const_cast<char*>("Terminated");
    case SIGINT: return const_cast<char*>("Interrupt");
    default:
      snprintf(buf, sizeof(buf), "Unknown signal %d", sig);
      return buf;
  }
}

// dl_iterate_phdr reports every loaded Windows module (the main executable
// first, where libart is statically linked) with one synthetic PT_LOAD
// program header spanning the module's image. ART uses this to find the
// range of libart's own code (OatQuickMethodHeader::IsStub, which the stack
// walker calls for every quick/nterp frame) and to symbolize addresses. ELF
// objects the gesher loader mapped report their real headers through
// <link.h>'s loader-side iteration (compat/bionic/shim.cc); they are not
// Windows modules and do not appear here.
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
  HMODULE mods[512];
  DWORD needed = 0;
  HANDLE proc = GetCurrentProcess();
  if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) return 0;
  size_t count = needed / sizeof(HMODULE);
  if (count > 512) count = 512;
  // One persistent PT_LOAD phdr per module (the callback keeps no copy, but
  // the pointer must stay valid for the call).
  static thread_local Elf64_Phdr phdr;
  for (size_t i = 0; i < count; ++i) {
    auto base = reinterpret_cast<uintptr_t>(mods[i]);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
    phdr = {};
    phdr.p_type = PT_LOAD;
    phdr.p_flags = PF_R | PF_X | PF_W;
    phdr.p_vaddr = 0;
    phdr.p_memsz = nt->OptionalHeader.SizeOfImage;
    char name[MAX_PATH] = "";
    GetModuleFileNameA(mods[i], name, sizeof(name));
    struct dl_phdr_info info = {};
    info.dlpi_addr = base;
    info.dlpi_name = name;
    info.dlpi_phdr = &phdr;
    info.dlpi_phnum = 1;
    int r = callback(&info, sizeof(info), data);
    if (r != 0) return r;
  }
  return 0;
}

}  // extern "C"
