// gesher-base: <unistd.h> and <stdlib.h> additions.

#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <atomic>

#include "internal.h"

namespace {

// Android gives each app its own uid; Gesher reports the first app uid.
constexpr uid_t kAppUid = 10000;

std::atomic<unsigned long long> rand_state{0x853c49e6748fea9bULL};

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
    case _SC_CLK_TCK:
      return 100;
    case _SC_OPEN_MAX:
      return 8192;
    default:
      errno = EINVAL;
      return -1;
  }
}

int getpagesize(void) { return static_cast<int>(sysconf(_SC_PAGESIZE)); }

uid_t getuid(void) { return kAppUid; }
uid_t geteuid(void) { return kAppUid; }
gid_t getgid(void) { return kAppUid; }
gid_t getegid(void) { return kAppUid; }
pid_t gettid(void) { return static_cast<pid_t>(GetCurrentThreadId()); }
pid_t getppid(void) { return 1; }
pid_t setsid(void) { return static_cast<pid_t>(GetCurrentProcessId()); }

// One process group per process.
int setpgid(pid_t, pid_t) { return 0; }
pid_t getpgid(pid_t) { return static_cast<pid_t>(GetCurrentProcessId()); }
pid_t getpgrp(void) { return static_cast<pid_t>(GetCurrentProcessId()); }

pid_t fork(void) { return errno = ENOSYS, -1; }

int pipe2(int fds[2], int flags) {
  int mode = _O_BINARY | ((flags & O_CLOEXEC) != 0 ? _O_NOINHERIT : 0);
  return _pipe(fds, 64 * 1024, mode);
}

int pipe(int fds[2]) { return pipe2(fds, 0); }

int fsync(int fd) { return _commit(fd); }
int fdatasync(int fd) { return _commit(fd); }

// Note: unlike Linux, these move the file position (Windows has no positioned
// I/O that leaves it alone on synchronous handles).
ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
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

// Gesher does not create symlinks, so nothing is one.
ssize_t readlink(const char*, char*, size_t) { return errno = EINVAL, -1; }
int symlink(const char*, const char*) { return errno = EPERM, -1; }

int link(const char* oldpath, const char* newpath) {
  if (!CreateHardLinkW(gesher::Widen(newpath).c_str(), gesher::Widen(oldpath).c_str(), nullptr)) {
    return gesher::FailWithLastError();
  }
  return 0;
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

char* realpath(const char* path, char* resolved_path) {
  std::wstring wpath = gesher::Widen(path);
  HANDLE h = CreateFileW(wpath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    gesher::FailWithLastError();
    return nullptr;
  }
  wchar_t buf[32768];
  DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(h);
  if (n == 0 || n >= 32768) {
    errno = ENAMETOOLONG;
    return nullptr;
  }
  const wchar_t* p = buf;
  if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;  // drop the \\?\ prefix
  std::string out = gesher::Narrow(p);
  for (char& c : out) {
    if (c == '\\') c = '/';
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
