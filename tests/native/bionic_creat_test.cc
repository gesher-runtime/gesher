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
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

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

namespace {
// Descriptor-relative operations must keep the admitted directory, even if
// its former name is replaced before the next operation.
void DirectoryContracts() {
#ifdef _WIN32
  const auto lookup = gesher::bionic::LookupBionicSymbol;
  using FdOpen = void*(SYSV*)(int);
  using DirFd = int(SYSV*)(void*);
  using DirClose = int(SYSV*)(void*);
  using DirRead = void*(SYSV*)(void*);
  using DirTell = int64_t(SYSV*)(void*);
  using DirSeek = void(SYSV*)(void*, int64_t);
  using OpenAt = int(SYSV*)(int, const char*, int, unsigned);
  using StatAt = int(SYSV*)(int, const char*, struct stat*, int);
  using DeleteAt = int(SYSV*)(int, const char*, int);
  auto fdopen = reinterpret_cast<FdOpen>(lookup("fdopendir"));
  auto dirfd_call = reinterpret_cast<DirFd>(lookup("dirfd"));
  auto dirclose = reinterpret_cast<DirClose>(lookup("closedir"));
  auto dirread = reinterpret_cast<DirRead>(lookup("readdir"));
  auto dirtell = reinterpret_cast<DirTell>(lookup("telldir"));
  auto dirseek = reinterpret_cast<DirSeek>(lookup("seekdir"));
  auto openat_call = reinterpret_cast<OpenAt>(lookup("openat"));
  auto statat = reinterpret_cast<StatAt>(lookup("fstatat"));
  auto deleteat = reinterpret_cast<DeleteAt>(lookup("unlinkat"));
  if (!Check("directory export routes resolve", fdopen && dirfd_call && dirclose && dirread && dirtell && dirseek && openat_call && statat && deleteat)) return;
#else
  auto fdopen = ::fdopendir;
  auto dirfd_call = ::dirfd;
  auto dirclose = ::closedir;
  auto dirread = ::readdir;
  auto dirtell = ::telldir;
  auto dirseek = ::seekdir;
  auto openat_call = [](int fd, const char* p, int f, unsigned m) { return ::openat(fd, p, f, m); };
  auto statat = ::fstatat;
  auto deleteat = ::unlinkat;
#endif
  char old[128], moved[128], child[160];
  std::snprintf(old, sizeof(old), "gesher-directory-%ld", static_cast<long>(getpid()));
  std::snprintf(moved, sizeof(moved), "%s-moved", old);
  if (!Check("create held directory", mkdir(old, 0700) == 0)) return;
  int parent = open(old, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (!Check("open held directory", parent >= 0)) { rmdir(old); return; }
  int file = openat_call(parent, "child", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  if (Check("create through actual held directory", file >= 0)) {
    Check("write child data", write(file, "held", 4) == 4);
    close(file);
  }
  const int adopted = dup(parent);
#ifdef _WIN32
  // Native getdents cookies are 64-bit entry positions. Adopt a descriptor
  // already past its first entry, rather than testing only a zero cursor.
  Check("advance directory descriptor before adoption", lseek(adopted, 1, SEEK_SET) == 1);
#endif
  auto* stream = fdopen(adopted);
  if (Check("fdopendir consumes actual descriptor", stream != nullptr)) {
    Check("dirfd returns same descriptor", dirfd_call(stream) == adopted);
#ifdef _WIN32
    Check("initial telldir retains adopted descriptor position", dirtell(stream) == 1);
#endif
    auto* first = dirread(stream);
    Check("read from adopted directory position", first != nullptr);
#ifdef _WIN32
    Check("first entry honors adopted cursor", first && std::strcmp(reinterpret_cast<const char*>(first) + 19, ".") != 0);
#endif
    const auto cookie = dirtell(stream);
#ifdef _WIN32
    Check("first entry advances actual adopted cookie", cookie > 1);
#endif
    auto* next = dirread(stream);
    // Both the Bionic and Linux dirent name begin after ino/off/reclen/type.
    const std::string expected = next ? reinterpret_cast<const char*>(next) + 19 : "";
    dirseek(stream, cookie);
    auto* repeated = dirread(stream);
    Check("nonzero adopted cursor cookie round trip", next && repeated && expected == reinterpret_cast<const char*>(repeated) + 19);
  }
  Check("rename held directory", rename(old, moved) == 0);
  Check("replace former name", mkdir(old, 0700) == 0);
  std::snprintf(child, sizeof(child), "%s/child", old);
  int replacement = open(child, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (replacement >= 0) { write(replacement, "other", 5); close(replacement); }
  // Bionic's stat conversion writes widened nanoseconds into this real host
  // stat layout; the child identity and size fields are common to both.
  struct stat st{};
  Check("statat follows held directory after replacement", statat(parent, "child", &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_size == 4);
  file = openat_call(parent, "child", O_RDONLY | O_CLOEXEC, 0);
  char bytes[5]{};
  Check("openat follows held directory after replacement", file >= 0 && read(file, bytes, 4) == 4 && std::strcmp(bytes, "held") == 0);
  if (file >= 0) close(file);
  file = openat_call(parent, "./child", O_RDONLY, 0);
  Check("dot component remains relative to held directory", file >= 0);
  if (file >= 0) close(file);
  file = openat_call(parent, ".", O_RDONLY | O_DIRECTORY, 0);
  Check("dot directory keeps existing resolver semantics", file >= 0);
  if (file >= 0) close(file);
  file = openat_call(parent, "..", O_RDONLY | O_DIRECTORY, 0);
  Check("parent traversal keeps existing resolver semantics", file >= 0);
  if (file >= 0) close(file);
  file = openat_call(parent, "child", O_WRONLY | O_APPEND | O_SYNC, 0);
  Check("relative append preserves data and status", file >= 0 && write(file, "!", 1) == 1 && (fcntl(file, F_GETFL) & O_APPEND));
  if (file >= 0) close(file);
  Check("relative append reached held child", statat(parent, "child", &st, 0) == 0 && st.st_size == 5);
  file = openat_call(parent, "child", O_WRONLY | O_TRUNC, 0);
  Check("relative truncate affects held child", file >= 0 && fstat(file, &st) == 0 && st.st_size == 0);
  if (file >= 0) close(file);
  std::snprintf(child, sizeof(child), "%s/fifo", moved);
  if (mkfifo(child, 0600) == 0) {
    file = openat_call(parent, "fifo", O_RDWR | O_NONBLOCK | O_TRUNC, 0);
    char byte = 0;
    Check("relative FIFO reaches actual queue", file >= 0 && write(file, "q", 1) == 1 && read(file, &byte, 1) == 1 && byte == 'q');
    if (file >= 0) {
      Check("relative FIFO retains FIFO metadata", fstat(file, &st) == 0 && S_ISFIFO(st.st_mode));
      close(file);
    }
    Check("relative FIFO deletion removes marker", deleteat(parent, "fifo", 0) == 0);
  } else std::puts("bionic_creat: FIFO capability unavailable on this filesystem");
  std::snprintf(child, sizeof(child), "%s/child", old);
  Check("unlinkat deletes held directory child", deleteat(parent, "child", 0) == 0);
  Check("replacement child remains intact", stat(child, &st) == 0 && st.st_size == 5);
  if (stream) {
    Check("closedir closes adopted descriptor", dirclose(stream) == 0);
    errno = 0;
    Check("adopted descriptor is closed exactly once", close(adopted) == -1 && errno == EBADF);
  } else close(adopted);
  Check("original descriptor remains owned by caller", fstat(parent, &st) == 0 && S_ISDIR(st.st_mode));
  close(parent);
#ifdef _WIN32
  const int virtual_fd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (Check("open real proc directory view", virtual_fd >= 0)) {
    Check("advance virtual directory cursor", lseek(virtual_fd, 1, SEEK_SET) == 1);
    auto* view = fdopen(virtual_fd);
    if (Check("adopt virtual directory view", view != nullptr)) {
      Check("virtual initial tell retains actual cursor", dirtell(view) == 1);
      auto* entry = dirread(view);
      Check("virtual first entry honors cursor", entry && std::strcmp(reinterpret_cast<const char*>(entry) + 19, ".") != 0);
      Check("virtual read advances real cookie", dirtell(view) > 1);
      const auto virtual_cookie = dirtell(view);
      entry = dirread(view);
      const std::string virtual_next = entry ? reinterpret_cast<const char*>(entry) + 19 : "";
      dirseek(view, virtual_cookie);
      entry = dirread(view);
      Check("virtual cookie round trip retains next entry", entry && virtual_next == reinterpret_cast<const char*>(entry) + 19);
      dirseek(view, 0);
      entry = dirread(view);
      Check("virtual rewind returns first entry", entry && std::strcmp(reinterpret_cast<const char*>(entry) + 19, ".") == 0);
      Check("close virtual directory view", dirclose(view) == 0);
    } else close(virtual_fd);
  }
#endif
  unlink(child);
  rmdir(old);
  rmdir(moved);
}
}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  Api api = Resolve();
  if (Check("resolve creat, creat64 and __errno", api.create && api.create64 && api.error)) {
    Contracts(api, api.create, "creat");
    Contracts(api, api.create64, "creat64");
    DirectoryContracts();
  }
  std::printf("bionic_creat: %d assertions, %d failures\n", assertions, failures);
  if (!failures) std::puts("bionic_creat_test all passed");
  return failures ? 1 : 0;
}
