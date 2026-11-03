// gesher bionic shim: Android's system CA directory under its Android names
// (docs/network.md, "Native TLS clients").
//
// Native TLS code that does not go through Conscrypt finds the system's trust
// anchors by path. Dart's dart:io on Android (Flutter) checks
// "/apex/com.android.conscrypt/cacerts" (opendir/readdir, newer SDKs) or
// "/system/etc/security/cacerts" (fstatat64), and hands the directory to
// BoringSSL's SSL_CTX_load_verify_locations, whose by-dir lookup fopen()s
// "<dir>/<subject hash>.<n>" for each issuer it needs. Gesher keeps that
// directory at $ANDROID_ROOT/etc/security/cacerts (compat/net/trust_store.cc
// writes it); a drive-less "/system/..." would be the current drive's root.
//
// WrapSystemCacerts wraps the path-taking libc entries in the shim's table
// (stat/lstat/fstatat/statfs/statvfs/access/faccessat/open/openat/fopen/
// freopen/opendir/realpath, and their 64/_2 aliases) so that exactly those two
// names, and the names under them, resolve to:
//
//   $ANDROID_ROOT/etc/security/cacerts   when that directory exists;
//   else cacerts\ next to the executable  (Android's own set, as shipped by
//                                          tools/net/collect.sh and the installer);
//   else $ANDROID_ROOT's (so the lookup fails with ENOENT).
//
// Only an exact component prefix is mapped ("/system/etc/security/cacertsX"
// is not); a name under it with "..", a backslash, a colon (drive or
// alternate data stream), a trailing dot or space, or a Win32 device name
// fails with ENOENT rather than escaping the directory. Relative names and
// *at() calls with a real dirfd are untouched: a dirfd opened on the mapped
// directory already is the Windows directory.
//
// Included by process_bionic.cc only (RegisterProcess runs last and calls
// WrapSystemCacerts after its own wrappers), so these definitions have
// internal linkage.

#ifndef GESHER_BIONIC_SYSTEM_CACERTS_H_
#define GESHER_BIONIC_SYSTEM_CACERTS_H_

#include "shim_internal.h"

#include <windows.h>

#include <errno.h>
#include <cstdlib>
#include <sys/stat.h>

#include <string>
#include <string_view>

namespace gesher::bionic {
namespace {
namespace system_cacerts {

constexpr std::string_view kNames[] = {
    "/system/etc/security/cacerts",
    // Android 14+ Conscrypt's copy; Dart prefers it when it has entries.
    "/apex/com.android.conscrypt/cacerts",
};

// `path` is one of kNames or under it (exact component boundary).
bool UnderName(std::string_view path, std::string_view* suffix) {
  for (std::string_view name : kNames) {
    if (path.size() >= name.size() && path.compare(0, name.size(), name) == 0 &&
        (path.size() == name.size() || path[name.size()] == '/')) {
      *suffix = path.substr(name.size());
      return true;
    }
  }
  return false;
}

// A suffix ("" or "/a/b") that stays inside the directory on Windows.
bool SafeSuffix(std::string_view suffix) {
  if (suffix.find_first_of("\\:") != std::string_view::npos) return false;
  size_t i = 0;
  while (i < suffix.size()) {
    if (suffix[i] == '/') {
      ++i;
      continue;
    }
    size_t end = suffix.find('/', i);
    if (end == std::string_view::npos) end = suffix.size();
    std::string_view part = suffix.substr(i, end - i);
    i = end;
    if (part == ".") continue;
    if (part == "..") return false;
    if (part.back() == '.' || part.back() == ' ') return false;
    // Win32 opens these basenames as devices, even with an extension.
    std::string stem(part.substr(0, part.find('.')));
    for (char& c : stem) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" ||
        stem == "CONOUT$") {
      return false;
    }
    if ((stem.size() == 4 || stem.size() == 5) &&
        (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)) {
      unsigned char d = static_cast<unsigned char>(stem[3]);
      if (stem.size() == 4 && d >= '1' && d <= '9') return false;
      // COM¹ COM² COM³ (UTF-8 superscripts) are devices too.
      if (stem.size() == 5 && d == 0xc2) {
        unsigned char e = static_cast<unsigned char>(stem[4]);
        if (e == 0xb9 || e == 0xb2 || e == 0xb3) return false;
      }
    }
  }
  return true;
}

bool IsDirectory(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// cacerts\ next to the executable, as a Gesher path ("/mnt/c/..." for a
// drive, "//server/share/..." for UNC); "" if the image path is unknown.
std::string BundledDirectory() {
  std::wstring image(512, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (n == 0) return std::string();
    if (n < image.size()) {
      image.resize(n);
      break;
    }
    if (image.size() >= 32768) return std::string();
    image.resize(image.size() * 2);
  }
  size_t slash = image.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::string();
  image.resize(slash);
  if (image.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    image = L"\\\\" + image.substr(8);
  } else if (image.rfind(L"\\\\?\\", 0) == 0) {
    image = image.substr(4);
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, image.data(), static_cast<int>(image.size()), nullptr,
                                0, nullptr, nullptr);
  if (len <= 0) return std::string();
  std::string dir(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, image.data(), static_cast<int>(image.size()), dir.data(), len,
                      nullptr, nullptr);
  for (char& c : dir) {
    if (c == '\\') c = '/';
  }
  if (dir.size() >= 2 && dir[1] == ':' &&
      ((dir[0] >= 'A' && dir[0] <= 'Z') || (dir[0] >= 'a' && dir[0] <= 'z'))) {
    char drive = static_cast<char>(dir[0] >= 'A' && dir[0] <= 'Z' ? dir[0] - 'A' + 'a' : dir[0]);
    dir = std::string("/mnt/") + drive + dir.substr(2);
  }
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  return dir + "/cacerts";
}

// Where the names map to (see the top of the file); "" if nowhere.
std::string Directory() {
  std::string primary;
  if (const char* root = std::getenv("ANDROID_ROOT"); root != nullptr && *root != '\0') {
    primary = root;
    while (primary.size() > 1 && (primary.back() == '/' || primary.back() == '\\')) {
      primary.pop_back();
    }
    primary += "/etc/security/cacerts";
    // ANDROID_ROOT=/system (Android's own value) would map onto itself.
    std::string_view ignored;
    if (UnderName(primary, &ignored)) primary.clear();
  }
  if (!primary.empty() && IsDirectory(primary)) return primary;
  std::string bundled = BundledDirectory();
  if (!bundled.empty() && IsDirectory(bundled)) return bundled;
  return primary;
}

// 0: not one of the names, use `path` as it is; 1: use *out; -1: refuse
// (ENOENT): a name under the directory that would escape it, or no directory.
int Map(const char* path, std::string* out) {
  if (path == nullptr || path[0] != '/') return 0;
  std::string_view suffix;
  if (!UnderName(path, &suffix)) return 0;
  if (!SafeSuffix(suffix)) return -1;
  std::string dir = Directory();
  if (dir.empty()) return -1;
  *out = dir;
  out->append(suffix.data(), suffix.size());
  return 1;
}

// The entry each wrapper delegates to, one slot per wrapped name.
template <int N>
struct Original {
  static inline void* fn = nullptr;
};

// MAPPED(path, failure): `q`, the name to pass on, or return `failure` (ENOENT).
#define GESHER_CACERTS_MAPPED(path, failure) \
  std::string mapped;                        \
  const char* q = (path);                    \
  switch (Map((path), &mapped)) {            \
    case 1: q = mapped.c_str(); break;       \
    case -1: errno = ENOENT; return failure; \
    default: break;                          \
  }                                          \
  if (Original<N>::fn == nullptr) return errno = ENOSYS, failure

// stat, lstat, statfs, statvfs (and their 64 names): (path, buffer).
template <int N>
int BIONIC_SYSV PathBuffer(const char* path, void* buffer) {
  GESHER_CACERTS_MAPPED(path, -1);
  return reinterpret_cast<int(BIONIC_SYSV*)(const char*, void*)>(Original<N>::fn)(q, buffer);
}
// access.
template <int N>
int BIONIC_SYSV PathInt(const char* path, int mode) {
  GESHER_CACERTS_MAPPED(path, -1);
  return reinterpret_cast<int(BIONIC_SYSV*)(const char*, int)>(Original<N>::fn)(q, mode);
}
// open, open64, __open_2 (whose mode is not read without O_CREAT).
template <int N>
int BIONIC_SYSV PathOpen(const char* path, int flags, int mode) {
  GESHER_CACERTS_MAPPED(path, -1);
  return reinterpret_cast<int(BIONIC_SYSV*)(const char*, int, int)>(Original<N>::fn)(q, flags,
                                                                                     mode);
}
// openat, openat64, __openat_2, faccessat: (dirfd, path, int, int). An
// absolute path makes the dirfd moot, as on Linux.
template <int N>
int BIONIC_SYSV AtInt(int dirfd, const char* path, int a, int b) {
  GESHER_CACERTS_MAPPED(path, -1);
  return reinterpret_cast<int(BIONIC_SYSV*)(int, const char*, int, int)>(Original<N>::fn)(
      dirfd, q, a, b);
}
// fstatat, fstatat64.
template <int N>
int BIONIC_SYSV AtBuffer(int dirfd, const char* path, void* buffer, int flags) {
  GESHER_CACERTS_MAPPED(path, -1);
  return reinterpret_cast<int(BIONIC_SYSV*)(int, const char*, void*, int)>(Original<N>::fn)(
      dirfd, q, buffer, flags);
}
// fopen, fopen64.
template <int N>
void* BIONIC_SYSV PathMode(const char* path, const char* mode) {
  GESHER_CACERTS_MAPPED(path, nullptr);
  return reinterpret_cast<void*(BIONIC_SYSV*)(const char*, const char*)>(Original<N>::fn)(q,
                                                                                           mode);
}
// freopen, freopen64 (a null path reopens the stream: passed on as it is).
template <int N>
void* BIONIC_SYSV PathModeStream(const char* path, const char* mode, void* stream) {
  GESHER_CACERTS_MAPPED(path, nullptr);
  return reinterpret_cast<void*(BIONIC_SYSV*)(const char*, const char*, void*)>(
      Original<N>::fn)(q, mode, stream);
}
// opendir.
template <int N>
void* BIONIC_SYSV PathOnly(const char* path) {
  GESHER_CACERTS_MAPPED(path, nullptr);
  return reinterpret_cast<void*(BIONIC_SYSV*)(const char*)>(Original<N>::fn)(q);
}
// realpath: the resolved name is the directory's real one.
template <int N>
char* BIONIC_SYSV PathResolved(const char* path, char* resolved) {
  GESHER_CACERTS_MAPPED(path, nullptr);
  return reinterpret_cast<char*(BIONIC_SYSV*)(const char*, char*)>(Original<N>::fn)(q, resolved);
}

#undef GESHER_CACERTS_MAPPED

}  // namespace system_cacerts

void WrapSystemCacerts(SymbolTable& t) {
  using namespace system_cacerts;
#define GESHER_CACERTS_WRAP(n, name, wrapper)                  \
  do {                                                         \
    auto it = t.find(name);                                    \
    if (it != t.end() && it->second != nullptr) {              \
      Original<n>::fn = it->second;                            \
      it->second = reinterpret_cast<void*>(&wrapper<n>);       \
    }                                                          \
  } while (0)
  GESHER_CACERTS_WRAP(0, "stat", PathBuffer);
  GESHER_CACERTS_WRAP(1, "stat64", PathBuffer);
  GESHER_CACERTS_WRAP(2, "lstat", PathBuffer);
  GESHER_CACERTS_WRAP(3, "lstat64", PathBuffer);
  GESHER_CACERTS_WRAP(4, "statfs", PathBuffer);
  GESHER_CACERTS_WRAP(5, "statfs64", PathBuffer);
  GESHER_CACERTS_WRAP(6, "statvfs", PathBuffer);
  GESHER_CACERTS_WRAP(7, "statvfs64", PathBuffer);
  GESHER_CACERTS_WRAP(8, "access", PathInt);
  GESHER_CACERTS_WRAP(9, "open", PathOpen);
  GESHER_CACERTS_WRAP(10, "open64", PathOpen);
  GESHER_CACERTS_WRAP(11, "__open_2", PathOpen);
  GESHER_CACERTS_WRAP(12, "openat", AtInt);
  GESHER_CACERTS_WRAP(13, "openat64", AtInt);
  GESHER_CACERTS_WRAP(14, "__openat_2", AtInt);
  GESHER_CACERTS_WRAP(15, "faccessat", AtInt);
  GESHER_CACERTS_WRAP(16, "fstatat", AtBuffer);
  GESHER_CACERTS_WRAP(17, "fstatat64", AtBuffer);
  GESHER_CACERTS_WRAP(18, "fopen", PathMode);
  GESHER_CACERTS_WRAP(19, "fopen64", PathMode);
  GESHER_CACERTS_WRAP(20, "freopen", PathModeStream);
  GESHER_CACERTS_WRAP(21, "freopen64", PathModeStream);
  GESHER_CACERTS_WRAP(22, "opendir", PathOnly);
  GESHER_CACERTS_WRAP(23, "realpath", PathResolved);
#undef GESHER_CACERTS_WRAP
}

}  // namespace
}  // namespace gesher::bionic

#endif  // GESHER_BIONIC_SYSTEM_CACERTS_H_
