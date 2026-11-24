// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// creat and creat64 through the real Bionic exports on Windows: what a .so
// imports by name (issue #320: a translated arm64 library called creat, and the
// missing export aborted the process). The same contract runs directly
// against Linux libc as the reference.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef _WIN32
#include <gesher/bionic_shim.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(_WIN32) && defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#else
#define SYSV
#endif

namespace {
using Creat = int(SYSV*)(const char*, unsigned);
using Error = int*(SYSV*)();
struct Api {
  Creat create;
  Creat create64;
  Error error;
};
int assertions, failures;

bool Check(const char* name, bool success) {
  ++assertions;
  if (!success) ++failures;
  std::printf("bionic_creat: %s %s\n", name, success ? "PASS" : "FAIL");
  return success;
}

#ifndef _WIN32
int* NativeError() { return &errno; }
int NativeCreat(const char* path, unsigned mode) { return ::creat(path, mode); }
int NativeCreat64(const char* path, unsigned mode) { return ::creat64(path, mode); }
#endif

Api Resolve() {
#ifdef _WIN32
  gesher::bionic::InstallBionicShim();
  auto lookup = gesher::bionic::LookupBionicSymbol;
  std::puts("bionic_creat: actual Bionic export route");
  return {reinterpret_cast<Creat>(lookup("creat")), reinterpret_cast<Creat>(lookup("creat64")),
          reinterpret_cast<Error>(lookup("__errno"))};
#else
  std::puts("bionic_creat: Linux libc reference route");
  return {NativeCreat, NativeCreat64, NativeError};
#endif
}

struct Result { int fd, error; };
Result Call(const Api& api, Creat call, const char* path, unsigned mode) {
  *api.error() = 0;
  int fd = call(path, mode);
  return {fd, *api.error()};
}

off_t Size(const char* path) {
  struct stat info {};
  return stat(path, &info) == 0 ? info.st_size : -1;
}

// A new file, then an existing one with contents: each call yields a
// write-only descriptor on an empty file, as open(O_CREAT|O_WRONLY|O_TRUNC).
void Contracts(const Api& api, Creat call, const char* label) {
  char path[64];
  std::snprintf(path, sizeof(path), "gesher-bionic-%s-%ld", label, static_cast<long>(getpid()));
  unlink(path);
  char name[128];

  Result made = Call(api, call, path, 0644);
  std::snprintf(name, sizeof(name), "%s creates a missing file", label);
  if (!Check(name, made.fd >= 0 && Size(path) == 0)) return;
  std::snprintf(name, sizeof(name), "%s descriptor writes", label);
  Check(name, write(made.fd, "abcdefgh", 8) == 8 && Size(path) == 8);
  char byte = 0;
  errno = 0;
  std::snprintf(name, sizeof(name), "%s descriptor is write-only (read: EBADF)", label);
  Check(name, read(made.fd, &byte, 1) == -1 && errno == EBADF);
  std::snprintf(name, sizeof(name), "%s descriptor is write-only (O_ACCMODE)", label);
  Check(name, (fcntl(made.fd, F_GETFL) & O_ACCMODE) == O_WRONLY);
  Check("close the first descriptor", close(made.fd) == 0);

  Result again = Call(api, call, path, 0644);
  std::snprintf(name, sizeof(name), "%s truncates an existing file", label);
  Check(name, again.fd >= 0 && Size(path) == 0);
  if (again.fd >= 0) {
    std::snprintf(name, sizeof(name), "%s writes from offset 0 after truncation", label);
    Check(name, write(again.fd, "xy", 2) == 2 && lseek(again.fd, 0, SEEK_CUR) == 2 &&
                    Size(path) == 2);
    Check("close the second descriptor", close(again.fd) == 0);
  }
  Check("remove the file", unlink(path) == 0);

  char missing[96];
  std::snprintf(missing, sizeof(missing), "gesher-bionic-%s-none-%ld/file", label,
                static_cast<long>(getpid()));
  Result absent = Call(api, call, missing, 0644);
  std::snprintf(name, sizeof(name), "%s in a missing directory fails with ENOENT", label);
  Check(name, absent.fd == -1 && absent.error == ENOENT);
  std::printf("bionic_creat: observed fd=%d errno=%d\n", absent.fd, absent.error);
}
}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Api api = Resolve();
  if (Check("resolve creat, creat64 and __errno", api.create && api.create64 && api.error)) {
    Contracts(api, api.create, "creat");
    Contracts(api, api.create64, "creat64");
  }
  std::printf("bionic_creat: %d assertions, %d failures\n", assertions, failures);
  if (!failures) std::puts("bionic_creat_test all passed");
  return failures ? 1 : 0;
}
