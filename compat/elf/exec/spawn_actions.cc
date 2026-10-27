// posix_spawn child actions over the actual bionic descriptor/directory APIs.
#include "spawn_actions.h"

#include <gesher/bionic_shim.h>

#include <fcntl.h>
#include <io.h>

namespace gesher::exec {

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
  if (!open || !close || !dup2 || !chdir || !fchdir || !fcntl) return 127;
  // The transport supplies NUL for absent standard handles so launcher startup
  // is safe. Restore the parent's closed slots before actions such as dup2.
  for (int fd = 0; fd < 3; ++fd) {
    bool present = false;
    for (const auto& f : request.fd_flags) present = present || f.first == fd;
    if (!present) close(fd);
  }
  for (const auto& f : request.fd_flags) {
    if (fcntl(f.first, F_SETFD, f.second) < 0) return 127;
  }
  auto duplicate = [&](int from, int to) {
    if (from != to && dup2(from, to) < 0) return false;
    int flags = fcntl(to, F_GETFD, 0);
    return flags >= 0 && fcntl(to, F_SETFD, flags & ~FD_CLOEXEC) == 0;
  };
  for (const SpawnAction& a : request.actions) {
    switch (a.what) {
      case 0: {
        int fd = open(a.path.c_str(), a.flags, static_cast<int>(a.mode));
        if (fd < 0) return 127;
        if (fd != a.new_fd) {
          if (!duplicate(fd, a.new_fd)) return 127;
          close(fd);
        }
        break;
      }
      case 1:
        close(a.fd);  // bionic deliberately ignores a failed close.
        break;
      case 2:
        if (!duplicate(a.fd, a.new_fd)) return 127;
        break;
      case 3:
        if (chdir(a.path.c_str()) < 0) return 127;
        break;
      case 4:
        if (fchdir(a.fd) < 0) return 127;
        break;
    }
  }
  // The existing UCRT inherited-fd protocol supports descriptors below7000.
  // Apply actions before closing these, so an explicit dup can retain one.
  for (int fd = 0; fd < 7000; ++fd) {
    intptr_t value = _get_osfhandle(fd);
    if (value == -1 || value == -2 || value == 0) continue;
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return 127;
    if ((flags & FD_CLOEXEC) != 0) close(fd);
  }
  return 0;
}

}  // namespace gesher::exec
