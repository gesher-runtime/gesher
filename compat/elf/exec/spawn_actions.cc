// posix_spawn child actions over the actual bionic descriptor/directory APIs.
#include "spawn_actions.h"
#include "exec_spawn.h"

#include <gesher/bionic_shim.h>

#include <fcntl.h>
#include <io.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace gesher::exec {

int SpawnFailure(const char* operation, const char* path, int fd, int new_fd, int action) {
  const DWORD win_error = GetLastError();
  const int error = errno;
  if (std::getenv("GESHER_SPAWN_TRACE") != nullptr) {
    wchar_t current[32768];
    DWORD n = GetCurrentDirectoryW(32768, current);
    std::string cwd = n > 0 && n < 32768 ? Narrow(std::wstring(current, n)) : "<unavailable>";
    std::fprintf(stderr, "INFO exec4 child setup: %s action%d fd%d to%d errno%d win-raw%lu path[%s] cwd[%s]\n",
                 operation, action, fd, new_fd, error, win_error, path, cwd.c_str());
  }
  return 127;
}

int ApplySpawnActions(const SpawnRequest& request) {
  using Open = int (__attribute__((sysv_abi))*)(const char*, int, int);
  using Close = int (__attribute__((sysv_abi))*)(int);
  using Dup2 = int (__attribute__((sysv_abi))*)(int, int);
  using Chdir = int (__attribute__((sysv_abi))*)(const char*);
  using Fcntl = int (__attribute__((sysv_abi))*)(int, int, uint64_t);
  auto open = reinterpret_cast<Open>(gesher::bionic::LookupBionicSymbol("open"));
  auto close = reinterpret_cast<Close>(gesher::bionic::LookupBionicSymbol("close"));
  auto dup2 = reinterpret_cast<Dup2>(gesher::bionic::LookupBionicSymbol("dup2"));
  auto chdir = reinterpret_cast<Chdir>(gesher::bionic::LookupBionicSymbol("chdir"));
  auto fchdir = reinterpret_cast<Close>(gesher::bionic::LookupBionicSymbol("fchdir"));
  auto fcntl = reinterpret_cast<Fcntl>(gesher::bionic::LookupBionicSymbol("fcntl"));
  if (!open || !close || !dup2 || !chdir || !fchdir || !fcntl) return SpawnFailure("symbol lookup");
  // The transport supplies NUL for absent standard handles so launcher startup
  // is safe. Restore the parent's closed slots before actions such as dup2.
  for (int fd = 0; fd < 3; ++fd) {
    bool present = false;
    for (const auto& f : request.fd_flags) present = present || f.first == fd;
    if (!present) close(fd);
  }
  for (const auto& f : request.fd_flags) {
    if (fcntl(f.first, F_SETFD, f.second) < 0) return SpawnFailure("restore flags", "", f.first);
  }
  auto duplicate = [&](int from, int to) {
    if (from != to && dup2(from, to) < 0) return false;
    int flags = fcntl(to, F_GETFD, 0);
    return flags >= 0 && fcntl(to, F_SETFD, flags & ~FD_CLOEXEC) == 0;
  };
  int index = -1;
  for (const SpawnAction& a : request.actions) {
    ++index;
    switch (a.what) {
      case 0: {
        int fd = open(a.path.c_str(), a.flags, static_cast<int>(a.mode));
        if (fd < 0) return SpawnFailure("open", a.path.c_str(), a.fd, a.new_fd, index);
        if (fd != a.new_fd) {
          if (!duplicate(fd, a.new_fd)) return SpawnFailure("open duplicate", a.path.c_str(), fd, a.new_fd, index);
          close(fd);
        }
        break;
      }
      case 1:
        close(a.fd);  // bionic deliberately ignores a failed close.
        break;
      case 2:
        if (!duplicate(a.fd, a.new_fd)) return SpawnFailure("duplicate", "", a.fd, a.new_fd, index);
        break;
      case 3:
        if (chdir(a.path.c_str()) < 0) return SpawnFailure("chdir", a.path.c_str(), -1, -1, index);
        break;
      case 4:
        if (fchdir(a.fd) < 0) return SpawnFailure("fchdir", "", a.fd, -1, index);
        break;
    }
  }
  // The existing UCRT inherited-fd protocol supports descriptors below7000.
  // Apply actions before closing these, so an explicit dup can retain one.
  for (int fd = 0; fd < 7000; ++fd) {
    intptr_t value = _get_osfhandle(fd);
    if (value == -1 || value == -2 || value == 0) continue;
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return SpawnFailure("final flags", "", fd);
    if ((flags & FD_CLOEXEC) != 0) close(fd);
  }
  return 0;
}

}  // namespace gesher::exec
