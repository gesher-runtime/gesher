// gesher bionic shim: directories, the *at() file calls, file-system queries
// and the rest of <unistd.h> an app's .so (and libc++'s std::filesystem)
// imports, over gesher-base (compat/posix) and the CRT.
//
// What differs between bionic's LP64 ABI and gesher-base is converted here:
// struct dirent (bionic: 256-byte d_name, 280 bytes; gesher-base's holds
// longer UTF-8 names), struct statvfs (bionic has 24 reserved bytes more),
// struct timespec (64-bit tv_nsec), sysconf's and pathconf's name numbers
// (bionic's own, not glibc's, which gesher-base uses), and fcntl's pointer
// argument. A DIR from fdopendir owns its fd, as on Linux; dirfd opens one
// on demand.

#include "shim_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// bcrypt.h needs windows.h first.
#include <bcrypt.h>

#include <direct.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gesher::bionic {

static_assert(sizeof(off_t) == 8, "gesher-base builds with 64-bit off_t");

namespace {

constexpr int kAtFdcwd = -100;
constexpr int kAtRemovedir = 0x200;
constexpr int kAtEmptyPath = 0x1000;
constexpr int kODirectory = 0200000;

// --- paths relative to a directory fd -----------------------------------------

bool IsAbsolute(const char* p) {
  return p[0] == '/' || p[0] == '\\' || (p[0] != 0 && p[1] == ':');
}

// The directory an fd refers to, as a path gesher-base's calls accept.
bool PathOfFd(int fd, std::string* out) {
  auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE || h == nullptr) return errno = kEBADF, false;
  wchar_t buf[32768];
  DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) return errno = kEBADF, false;
  std::wstring w(buf, n);
  if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) {
    w = L"\\\\" + w.substr(8);
  } else if (w.rfind(L"\\\\?\\", 0) == 0) {
    w = w.substr(4);
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
  out->assign(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out->data(), len, nullptr,
                      nullptr);
  for (char& c : *out) {
    if (c == '\\') c = '/';
  }
  return true;
}

std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// `path` relative to `dirfd` (AT_FDCWD: the current directory).
bool AtPath(int dirfd, const char* path, std::string* out) {
  if (path == nullptr) return errno = kEFAULT, false;
  if (dirfd == kAtFdcwd || IsAbsolute(path)) {
    *out = path;
    return true;
  }
  if (!PathOfFd(dirfd, out)) return false;
  if (*path != 0) {
    if (out->empty() || out->back() != '/') *out += '/';
    *out += path;
  }
  return true;
}

// --- directories -----------------------------------------------------------------

// bionic's struct dirent (LP64).
struct BionicDirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};
static_assert(sizeof(BionicDirent) == 280);

struct ShimDir {
  DIR* host = nullptr;
  int fd = -1;  // owned: from fdopendir, or opened by dirfd
  std::string path;
  BionicDirent entry{};
  std::mutex mutex;
};

ShimDir* BIONIC_SYSV Opendir(const char* name) {
  DIR* host = opendir(name);
  if (host == nullptr) return nullptr;
  auto* d = new ShimDir();
  d->host = host;
  d->path = name;
  return d;
}
ShimDir* BIONIC_SYSV Fdopendir(int fd) {
  std::string path;
  if (!PathOfFd(fd, &path)) return nullptr;
  ShimDir* d = Opendir(path.c_str());
  if (d != nullptr) d->fd = fd;
  return d;
}
bool Fill(ShimDir* d, BionicDirent* out) {
  struct dirent* e = readdir(d->host);
  if (e == nullptr) return false;
  out->d_ino = e->d_ino != 0 ? e->d_ino : static_cast<uint64_t>(e->d_off);
  out->d_off = e->d_off;
  out->d_reclen = sizeof(BionicDirent);
  out->d_type = e->d_type;
  size_t n = std::strlen(e->d_name);
  if (n > 255) n = 255;  // a name longer than Linux allows (UTF-8 of 255 UTF-16 units)
  std::memcpy(out->d_name, e->d_name, n);
  out->d_name[n] = 0;
  return true;
}
BionicDirent* BIONIC_SYSV Readdir(ShimDir* d) {
  if (d == nullptr) return errno = kEBADF, nullptr;
  std::lock_guard<std::mutex> lock(d->mutex);
  return Fill(d, &d->entry) ? &d->entry : nullptr;
}
int BIONIC_SYSV ReaddirR(ShimDir* d, BionicDirent* entry, BionicDirent** result) {
  std::lock_guard<std::mutex> lock(d->mutex);
  int saved = errno;
  errno = 0;
  bool ok = Fill(d, entry);
  int e = errno;
  errno = saved;
  *result = ok ? entry : nullptr;
  return ok ? 0 : e;
}
int BIONIC_SYSV Closedir(ShimDir* d) {
  if (d == nullptr) return errno = kEBADF, -1;
  closedir(d->host);
  if (d->fd >= 0) close(d->fd);
  delete d;
  return 0;
}
void BIONIC_SYSV Rewinddir(ShimDir* d) {
  std::lock_guard<std::mutex> lock(d->mutex);
  rewinddir(d->host);
}
int BIONIC_SYSV Dirfd(ShimDir* d) {
  std::lock_guard<std::mutex> lock(d->mutex);
  if (d->fd < 0) d->fd = open(d->path.c_str(), O_RDONLY | kODirectory | O_CLOEXEC);
  return d->fd;
}

// scandir with the .so's System V filter and comparator; alphasort is the
// shim's, so the .so may pass it as the comparator.
using SysvFilter = int(BIONIC_SYSV*)(const BionicDirent*);
using SysvDirCompare = int(BIONIC_SYSV*)(const BionicDirent**, const BionicDirent**);
int BIONIC_SYSV Alphasort(const BionicDirent** a, const BionicDirent** b) {
  return std::strcmp((*a)->d_name, (*b)->d_name);
}
int BIONIC_SYSV Scandir(const char* path, BionicDirent*** list, SysvFilter filter,
                        SysvDirCompare compare) {
  ShimDir* d = Opendir(path);
  if (d == nullptr) return -1;
  std::vector<BionicDirent*> entries;
  BionicDirent e;
  while (Fill(d, &e)) {
    if (filter != nullptr && filter(&e) == 0) continue;
    auto* copy = static_cast<BionicDirent*>(std::malloc(sizeof(e)));
    *copy = e;
    entries.push_back(copy);
  }
  Closedir(d);
  if (compare != nullptr) {
    // Insertion sort through the System V comparator (lists are short).
    for (size_t i = 1; i < entries.size(); ++i) {
      for (size_t j = i; j > 0; --j) {
        const BionicDirent* x = entries[j - 1];
        const BionicDirent* y = entries[j];
        if (compare(&x, &y) <= 0) break;
        std::swap(entries[j - 1], entries[j]);
      }
    }
  }
  auto** out = static_cast<BionicDirent**>(std::malloc((entries.size() + 1) * sizeof(void*)));
  for (size_t i = 0; i < entries.size(); ++i) out[i] = entries[i];
  *list = out;
  return static_cast<int>(entries.size());
}

// --- the *at() calls and other path calls ---------------------------------------

// gesher-base's struct stat with its 32-bit tv_nsec widened (shim.cc's FixStat).
void WidenStat(struct stat* st) {
  for (struct timespec* ts : {&st->st_atim, &st->st_mtim, &st->st_ctim}) {
    int64_t ns = ts->tv_nsec;
    std::memcpy(reinterpret_cast<uint8_t*>(ts) + 8, &ns, 8);
  }
}

int BIONIC_SYSV Openat(int dirfd, const char* path, int flags, int mode) {
  return openat(dirfd, path, flags, mode);
}
int BIONIC_SYSV Fstatat(int dirfd, const char* path, struct stat* st, int flags) {
  std::memset(st, 0, sizeof(*st));
  int r;
  if ((flags & kAtEmptyPath) != 0 && path != nullptr && path[0] == 0) {
    r = fstat(dirfd, st);
  } else {
    std::string p;
    if (!AtPath(dirfd, path, &p)) return -1;
    r = (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) != 0 ? lstat(p.c_str(), st) : stat(p.c_str(), st);
  }
  if (r == 0) WidenStat(st);
  return r;
}
// unlink/rmdir with Linux's semantics: the name goes at once, even while the
// file (or directory) is open, as long as the opener shared delete access
// (gesher-base's open does; the shim's fopen goes through it). A plain
// DeleteFile/RemoveDirectory only marks it delete-pending, which keeps the
// name, and its parent non-empty, until the last handle closes; std::
// filesystem's remove_all removes a directory while it still holds it open.
// Falls back to gesher-base's calls (and their errno) when the POSIX
// disposition is unavailable.
int PosixDelete(const char* path, bool directory) {
  if (path == nullptr) return errno = kEFAULT, -1;
  std::wstring w = Wide(path);
  HANDLE h = CreateFileW(w.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                         nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    bool is_dir = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                  (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                  (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (is_dir != directory) {
      CloseHandle(h);
      return errno = directory ? kENOTDIR : 21 /* EISDIR */, -1;
    }
    FILE_DISPOSITION_INFO_EX info{};
    info.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                 FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    bool ok = SetFileInformationByHandle(h, FileDispositionInfoEx, &info, sizeof(info));
    DWORD error = GetLastError();
    CloseHandle(h);
    if (ok) return 0;
    if (error == ERROR_DIR_NOT_EMPTY) return errno = 39 /* ENOTEMPTY */, -1;
  }
  return directory ? rmdir(path) : unlink(path);
}

int BIONIC_SYSV Unlink(const char* path) { return PosixDelete(path, false); }
int BIONIC_SYSV Rmdir(const char* path) { return PosixDelete(path, true); }
int BIONIC_SYSV Remove(const char* path) {
  struct stat st {};
  if (path != nullptr && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return Rmdir(path);
  return Unlink(path);
}
int BIONIC_SYSV Unlinkat(int dirfd, const char* path, int flags) {
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return PosixDelete(p.c_str(), (flags & kAtRemovedir) != 0);
}
int BIONIC_SYSV Mkdirat(int dirfd, const char* path, uint32_t mode) {
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return mkdir(p.c_str(), static_cast<mode_t>(mode));
}
int BIONIC_SYSV Renameat(int olddir, const char* oldpath, int newdir, const char* newpath) {
  std::string a, b;
  if (!AtPath(olddir, oldpath, &a) || !AtPath(newdir, newpath, &b)) return -1;
  // Linux replaces an existing target; the CRT's rename does not.
  std::wstring wa = Wide(a), wb = Wide(b);
  if (MoveFileExW(wa.c_str(), wb.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    return 0;
  }
  return std::rename(a.c_str(), b.c_str());
}
int BIONIC_SYSV Rename(const char* a, const char* b) { return Renameat(kAtFdcwd, a, kAtFdcwd, b); }
int BIONIC_SYSV Faccessat(int dirfd, const char* path, int mode, int) {
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return access(p.c_str(), mode);
}
int BIONIC_SYSV Chmod(const char* path, uint32_t mode) {
  // The CRT models only the owner's write bit (read-only files).
  return _chmod(path, (mode & 0200) != 0 ? (_S_IREAD | _S_IWRITE) : _S_IREAD) == 0 ? 0 : -1;
}
int BIONIC_SYSV Fchmodat(int dirfd, const char* path, uint32_t mode, int) {
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return Chmod(p.c_str(), mode);
}
int BIONIC_SYSV Fchmod(int fd, uint32_t mode) { return fchmod(fd, static_cast<mode_t>(mode)); }
int BIONIC_SYSV Chown(const char* path, uint32_t uid, uint32_t gid) { return chown(path, uid, gid); }
int BIONIC_SYSV Fchown(int fd, uint32_t uid, uint32_t gid) { return fchown(fd, uid, gid); }
int BIONIC_SYSV Fchownat(int dirfd, const char* path, uint32_t uid, uint32_t gid, int) {
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return chown(p.c_str(), uid, gid);
}

bool NarrowTimes(const LinuxTimespec* in, struct timespec out[2]) {
  if (in == nullptr) return false;
  for (int i = 0; i < 2; ++i) {
    out[i].tv_sec = static_cast<time_t>(in[i].tv_sec);
    out[i].tv_nsec = static_cast<long>(in[i].tv_nsec);
  }
  return true;
}
int BIONIC_SYSV Utimensat(int dirfd, const char* path, const LinuxTimespec* times, int flags) {
  struct timespec ts[2];
  bool have = NarrowTimes(times, ts);
  if (path == nullptr) return futimens(dirfd, have ? ts : nullptr);
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return utimensat(kAtFdcwd, p.c_str(), have ? ts : nullptr, flags);
}
int BIONIC_SYSV Futimens(int fd, const LinuxTimespec* times) {
  struct timespec ts[2];
  return futimens(fd, NarrowTimes(times, ts) ? ts : nullptr);
}

char* BIONIC_SYSV Getcwd(char* buf, size_t size) {
  return getcwd(buf, static_cast<int>(size > 0x7fffffff ? 0x7fffffff : size));
}
int BIONIC_SYSV Chdir(const char* path) { return chdir(path); }
int BIONIC_SYSV Fchdir(int fd) {
  std::string p;
  if (!PathOfFd(fd, &p)) return -1;
  return Chdir(p.c_str());
}
char* BIONIC_SYSV Realpath(const char* path, char* resolved) { return realpath(path, resolved); }
int64_t BIONIC_SYSV Readlink(const char* path, char* buf, size_t n) { return readlink(path, buf, n); }
int64_t BIONIC_SYSV Readlinkat(int dirfd, const char* path, char* buf, size_t n) {
  std::string p;
  if (!AtPath(dirfd, path, &p)) return -1;
  return readlink(p.c_str(), buf, n);
}
int BIONIC_SYSV Symlink(const char* target, const char* link_path) {
  return symlink(target, link_path);
}
int BIONIC_SYSV Symlinkat(const char* target, int dirfd, const char* link_path) {
  std::string p;
  if (!AtPath(dirfd, link_path, &p)) return -1;
  return symlink(target, p.c_str());
}
int BIONIC_SYSV Link(const char* a, const char* b) { return link(a, b); }
int BIONIC_SYSV Linkat(int olddir, const char* a, int newdir, const char* b, int) {
  std::string x, y;
  if (!AtPath(olddir, a, &x) || !AtPath(newdir, b, &y)) return -1;
  return link(x.c_str(), y.c_str());
}
int BIONIC_SYSV Ftruncate(int fd, int64_t length) {
  if (length < 0) return errno = kEINVAL, -1;
  int rc = _chsize_s(fd, length);
  return rc == 0 ? 0 : (errno = rc == EBADF ? kEBADF : kEINVAL, -1);
}
int BIONIC_SYSV Truncate(const char* path, int64_t length) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  int rc = Ftruncate(fd, length);
  int saved = errno;
  close(fd);
  errno = saved;
  return rc;
}
int BIONIC_SYSV Fsync(int fd) { return fsync(fd); }
int BIONIC_SYSV Fdatasync(int fd) { return fdatasync(fd); }
uint32_t BIONIC_SYSV Umask(uint32_t mask) { return static_cast<uint32_t>(_umask(static_cast<int>(mask & 0777))); }
int BIONIC_SYSV Mkfifo(const char*, uint32_t) { return errno = kEPERM, -1; }

int64_t BIONIC_SYSV Sendfile(int out_fd, int in_fd, int64_t* offset, size_t count) {
  return sendfile(out_fd, in_fd, reinterpret_cast<off_t*>(offset), count);
}

// bionic's struct statvfs: gesher-base's fields plus 24 reserved bytes.
struct BionicStatvfs {
  struct statvfs base;
  uint32_t reserved[6];
};
int BIONIC_SYSV Statvfs(const char* path, BionicStatvfs* out) {
  std::memset(out, 0, sizeof(*out));
  return statvfs(path, &out->base);
}
int BIONIC_SYSV Fstatvfs(int fd, BionicStatvfs* out) {
  std::memset(out, 0, sizeof(*out));
  return fstatvfs(fd, &out->base);
}

// pathconf/fpathconf, by bionic's _PC_* numbers.
int64_t PathconfValue(int name) {
  switch (name) {
    case 0: return 64;      // _PC_FILESIZEBITS
    case 1: return 1024;    // _PC_LINK_MAX
    case 2: return 255;     // _PC_MAX_CANON
    case 3: return 255;     // _PC_MAX_INPUT
    case 4: return 255;     // _PC_NAME_MAX
    case 5: return 4096;    // _PC_PATH_MAX
    case 6: return 4096;    // _PC_PIPE_BUF
    case 7: return 1;       // _PC_2_SYMLINKS
    case 8: case 9: case 10: case 11: case 12: return 4096;  // allocation/transfer sizes
    case 13: return -1;     // _PC_SYMLINK_MAX: no limit
    case 14: return 1;      // _PC_CHOWN_RESTRICTED
    case 15: return 1;      // _PC_NO_TRUNC
    case 16: return 0;      // _PC_VDISABLE
    case 17: case 18: case 19: return -1;  // async/prioritized/synchronized I/O
    default: return errno = kEINVAL, -1;
  }
}
int64_t BIONIC_SYSV Pathconf(const char*, int name) { return PathconfValue(name); }
int64_t BIONIC_SYSV Fpathconf(int, int name) { return PathconfValue(name); }

// sysconf, by bionic's _SC_* numbers (libc/include/bits/sysconf.h).
int64_t BIONIC_SYSV Sysconf(int name) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  auto memory = [](bool avail) {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    GlobalMemoryStatusEx(&status);
    return static_cast<int64_t>((avail ? status.ullAvailPhys : status.ullTotalPhys) / 4096);
  };
  switch (name) {
    case 0x00: return 32767;   // _SC_ARG_MAX (a Windows command line)
    case 0x01: return 99;      // _SC_BC_BASE_MAX
    case 0x02: return 2048;    // _SC_BC_DIM_MAX
    case 0x03: return 99;      // _SC_BC_SCALE_MAX
    case 0x04: return 1000;    // _SC_BC_STRING_MAX
    case 0x05: return -1;      // _SC_CHILD_MAX
    case 0x06: return 100;     // _SC_CLK_TCK
    case 0x07: return 2;       // _SC_COLL_WEIGHTS_MAX
    case 0x08: return 32;      // _SC_EXPR_NEST_MAX
    case 0x09: return 2048;    // _SC_LINE_MAX
    case 0x0a: return 65536;   // _SC_NGROUPS_MAX
    case 0x0b: return 8192;    // _SC_OPEN_MAX
    case 0x0c: return 128;     // _SC_PASS_MAX
    case 0x16: case 0x19: return 200809;  // _SC_2_VERSION, _SC_VERSION
    case 0x17: case 0x18: return 1;       // _SC_JOB_CONTROL, _SC_SAVED_IDS
    case 0x1a: return 255;     // _SC_RE_DUP_MAX
    case 0x1b: return 8;       // _SC_STREAM_MAX
    case 0x1c: return 6;       // _SC_TZNAME_MAX
    case 0x25: return 65536;   // _SC_ATEXIT_MAX
    case 0x26: return 1024;    // _SC_IOV_MAX
    case 0x27: case 0x28: return 4096;  // _SC_PAGESIZE, _SC_PAGE_SIZE
    case 0x47: case 0x48: return 1024;  // _SC_GETGR_R_SIZE_MAX, _SC_GETPW_R_SIZE_MAX
    case 0x49: return 256;     // _SC_LOGIN_NAME_MAX
    case 0x4a: return 4;       // _SC_THREAD_DESTRUCTOR_ITERATIONS
    case 0x4b: return 128;     // _SC_THREAD_KEYS_MAX
    case 0x4c: return 16384;   // _SC_THREAD_STACK_MIN
    case 0x4d: return -1;      // _SC_THREAD_THREADS_MAX
    case 0x4e: return 32;      // _SC_TTY_NAME_MAX
    case 0x4f: case 0x50: case 0x51: case 0x55: return 200809;  // _SC_THREADS & co.
    case 0x60: case 0x61:      // _SC_NPROCESSORS_CONF, _SC_NPROCESSORS_ONLN
      return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    case 0x62: return memory(false);  // _SC_PHYS_PAGES
    case 0x63: return memory(true);   // _SC_AVPHYS_PAGES
    case 0x64: return 200809;  // _SC_MONOTONIC_CLOCK
    case 0x6f: return 255;     // _SC_HOST_NAME_MAX
    case 0x94: case 0x97: case 0x9a: case 0x9d: return 64;  // cache line sizes
    default:
      if (name >= 0 && name <= 0x9e) return -1;  // known, no limit / unsupported
      return errno = kEINVAL, -1;
  }
}
int BIONIC_SYSV GetNprocs() { return static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)); }

// --- fcntl, ioctl ------------------------------------------------------------------

struct BionicFlock {
  int16_t l_type;
  int16_t l_whence;
  int64_t l_start;
  int64_t l_len;
  int32_t l_pid;
};

int BIONIC_SYSV Fcntl(int fd, int cmd, uint64_t arg) {
  switch (cmd) {
    case 5:   // F_GETLK
    case 36:  // F_OFD_GETLK
      // Advisory record locks are not modelled: nothing else holds one.
      reinterpret_cast<BionicFlock*>(arg)->l_type = 2;  // F_UNLCK
      return 0;
    case 6: case 7:     // F_SETLK, F_SETLKW
    case 37: case 38:   // F_OFD_SETLK, F_OFD_SETLKW
      return 0;
    default:
      return fcntl(fd, cmd, static_cast<long>(arg));
  }
}
int BIONIC_SYSV Ioctl(int fd, uint64_t request, void* arg) {
  return ioctl(fd, static_cast<unsigned long>(request), arg);
}

// --- identity and system --------------------------------------------------------------

uint32_t BIONIC_SYSV Getuid() { return getuid(); }
uint32_t BIONIC_SYSV Getgid() { return getgid(); }
int BIONIC_SYSV Getppid() { return getppid(); }
int BIONIC_SYSV Uname(struct utsname* u) { return uname(u); }
int BIONIC_SYSV Gethostname(char* name, size_t len) { return gesher_gethostname(name, len); }
int BIONIC_SYSV Getrlimit(int resource, uint64_t* rl) {
  return getrlimit(resource, reinterpret_cast<struct rlimit*>(rl));
}
int BIONIC_SYSV Setrlimit(int resource, const uint64_t* rl) {
  return setrlimit(resource, reinterpret_cast<const struct rlimit*>(rl));
}
int BIONIC_SYSV SchedGetcpu() { return static_cast<int>(GetCurrentProcessorNumber()); }
int BIONIC_SYSV Getpriority(int which, uint32_t who) { return getpriority(which, who); }
int BIONIC_SYSV Setpriority(int which, uint32_t who, int prio) {
  return setpriority(which, who, prio);
}

// getauxval: what code asks the auxiliary vector for.
alignas(16) uint8_t g_at_random[16];
uint64_t BIONIC_SYSV Getauxval(uint64_t type) {
  switch (type) {
    case 6: return 4096;                                     // AT_PAGESZ
    case 11: case 12: return getuid();                       // AT_UID, AT_EUID
    case 13: case 14: return getgid();                       // AT_GID, AT_EGID
    case 16: case 26: return 0;                              // AT_HWCAP, AT_HWCAP2
    case 17: return 100;                                     // AT_CLKTCK
    case 23: return 0;                                       // AT_SECURE
    case 25: return reinterpret_cast<uint64_t>(g_at_random);  // AT_RANDOM
    default: return errno = kENOENT, 0;
  }
}

// --- FORTIFY variants of the fd and FILE calls, and string functions ---------------

[[noreturn]] void FortifyFailure(const char* what) {
  std::fprintf(stderr, "gesher bionic: FORTIFY: %s: prevented buffer overflow\n", what);
  std::fflush(stderr);
  std::abort();
}
int64_t BIONIC_SYSV ReadChk(int fd, void* buf, size_t n, size_t buf_size) {
  if (n > buf_size) FortifyFailure("read");
  return read(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
}
int64_t BIONIC_SYSV WriteChk(int fd, const void* buf, size_t n, size_t buf_size) {
  if (n > buf_size) FortifyFailure("write");
  return write(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
}
int64_t BIONIC_SYSV PreadChk(int fd, void* buf, size_t n, int64_t off, size_t buf_size) {
  if (n > buf_size) FortifyFailure("pread");
  return pread(fd, buf, n, static_cast<off_t>(off));
}
int64_t BIONIC_SYSV PwriteChk(int fd, const void* buf, size_t n, int64_t off, size_t buf_size) {
  if (n > buf_size) FortifyFailure("pwrite");
  return pwrite(fd, buf, n, static_cast<off_t>(off));
}
char* BIONIC_SYSV GetcwdChk(char* buf, size_t n, size_t buf_size) {
  if (n > buf_size) FortifyFailure("getcwd");
  return Getcwd(buf, n);
}
int64_t BIONIC_SYSV ReadlinkChk(const char* path, char* buf, size_t n, size_t buf_size) {
  if (n > buf_size) FortifyFailure("readlink");
  return readlink(path, buf, n);
}
int64_t BIONIC_SYSV ReadlinkatChk(int dirfd, const char* path, char* buf, size_t n,
                                  size_t buf_size) {
  if (n > buf_size) FortifyFailure("readlinkat");
  return Readlinkat(dirfd, path, buf, n);
}
uint32_t BIONIC_SYSV UmaskChk(uint32_t mask) { return Umask(mask); }

char* BIONIC_SYSV Stpcpy(char* d, const char* s) {
  size_t n = std::strlen(s);
  std::memcpy(d, s, n + 1);
  return d + n;
}
char* BIONIC_SYSV Stpncpy(char* d, const char* s, size_t n) {
  size_t len = strnlen(s, n);
  std::memcpy(d, s, len);
  std::memset(d + len, 0, n - len);
  return d + len;
}
char* BIONIC_SYSV StpcpyChk(char* d, const char* s, size_t dn) {
  if (std::strlen(s) >= dn) FortifyFailure("stpcpy");
  return Stpcpy(d, s);
}
char* BIONIC_SYSV StpncpyChk(char* d, const char* s, size_t n, size_t dn) {
  if (n > dn) FortifyFailure("stpncpy");
  return Stpncpy(d, s, n);
}
void* BIONIC_SYSV Memrchr(const void* s, int c, size_t n) {
  auto p = static_cast<const uint8_t*>(s);
  while (n-- > 0) {
    if (p[n] == static_cast<uint8_t>(c)) return const_cast<uint8_t*>(p + n);
  }
  return nullptr;
}
void* BIONIC_SYSV MemchrChk(const void* s, int c, size_t n, size_t buf_size) {
  if (n > buf_size) FortifyFailure("memchr");
  return const_cast<void*>(std::memchr(s, c, n));
}
void* BIONIC_SYSV MemrchrChk(const void* s, int c, size_t n, size_t buf_size) {
  if (n > buf_size) FortifyFailure("memrchr");
  return Memrchr(s, c, n);
}
char* BIONIC_SYSV Strsep(char** sp, const char* delim) {
  char* s = *sp;
  if (s == nullptr) return nullptr;
  char* end = s + std::strcspn(s, delim);
  if (*end != 0) {
    *end = 0;
    *sp = end + 1;
  } else {
    *sp = nullptr;
  }
  return s;
}
char* BIONIC_SYSV Strcasestr(const char* h, const char* n) {
  size_t len = std::strlen(n);
  for (; *h; ++h) {
    if (_strnicmp(h, n, len) == 0) return const_cast<char*>(h);
  }
  return len == 0 ? const_cast<char*>(h) : nullptr;
}
char* BIONIC_SYSV Strchrnul(const char* s, int c) {
  while (*s && *s != static_cast<char>(c)) ++s;
  return const_cast<char*>(s);
}
char* BIONIC_SYSV Strtok(char* s, const char* delim) { return std::strtok(s, delim); }
// basename/dirname (POSIX: they may modify and return their argument or a
// static buffer; bionic's never modify it).
thread_local char t_path_buf[4096];
char* BIONIC_SYSV Basename(const char* path) {
  if (path == nullptr || *path == 0) return std::strcpy(t_path_buf, ".");
  std::string p(path);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t slash = p.find_last_of('/');
  std::string b = slash == std::string::npos || p == "/" ? p : p.substr(slash + 1);
  std::snprintf(t_path_buf, sizeof(t_path_buf), "%s", b.c_str());
  return t_path_buf;
}
char* BIONIC_SYSV Dirname(const char* path) {
  if (path == nullptr || *path == 0) return std::strcpy(t_path_buf, ".");
  std::string p(path);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t slash = p.find_last_of('/');
  std::string d = slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  std::snprintf(t_path_buf, sizeof(t_path_buf), "%s", d.c_str());
  return t_path_buf;
}

const char* g_progname = "app_process";

}  // namespace

void RegisterPosix(SymbolTable& t) {
  BCryptGenRandom(nullptr, g_at_random, sizeof(g_at_random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  E("opendir", Opendir);
  E("fdopendir", Fdopendir);
  E("readdir", Readdir);
  E("readdir64", Readdir);
  E("readdir_r", ReaddirR);
  E("readdir64_r", ReaddirR);
  E("closedir", Closedir);
  E("rewinddir", Rewinddir);
  E("dirfd", Dirfd);
  E("scandir", Scandir);
  E("scandir64", Scandir);
  E("alphasort", Alphasort);
  E("alphasort64", Alphasort);
  E("openat", Openat);
  E("openat64", Openat);
  E("__openat_2", Openat);
  E("fstatat", Fstatat);
  E("fstatat64", Fstatat);
  E("unlinkat", Unlinkat);
  E("rmdir", Rmdir);
  E("unlink", Unlink);
  E("remove", Remove);
  E("mkdirat", Mkdirat);
  E("renameat", Renameat);
  E("rename", Rename);
  E("faccessat", Faccessat);
  E("chmod", Chmod);
  E("fchmod", Fchmod);
  E("fchmodat", Fchmodat);
  E("chown", Chown);
  E("lchown", Chown);
  E("fchown", Fchown);
  E("fchownat", Fchownat);
  E("utimensat", Utimensat);
  E("futimens", Futimens);
  E("getcwd", Getcwd);
  E("chdir", Chdir);
  E("fchdir", Fchdir);
  E("realpath", Realpath);
  E("readlink", Readlink);
  E("readlinkat", Readlinkat);
  E("symlink", Symlink);
  E("symlinkat", Symlinkat);
  E("link", Link);
  E("linkat", Linkat);
  E("truncate", Truncate);
  E("truncate64", Truncate);
  E("ftruncate", Ftruncate);
  E("ftruncate64", Ftruncate);
  E("fsync", Fsync);
  E("fdatasync", Fdatasync);
  E("umask", Umask);
  E("mkfifo", Mkfifo);
  E("sendfile", Sendfile);
  E("sendfile64", Sendfile);
  E("statvfs", Statvfs);
  E("statvfs64", Statvfs);
  E("fstatvfs", Fstatvfs);
  E("fstatvfs64", Fstatvfs);
  E("pathconf", Pathconf);
  E("fpathconf", Fpathconf);
  E("sysconf", Sysconf);
  E("get_nprocs", GetNprocs);
  E("get_nprocs_conf", GetNprocs);
  E("fcntl", Fcntl);
  E("ioctl", Ioctl);
  E("getuid", Getuid);
  E("geteuid", Getuid);
  E("getgid", Getgid);
  E("getegid", Getgid);
  E("getppid", Getppid);
  E("uname", Uname);
  E("gethostname", Gethostname);
  E("getrlimit", Getrlimit);
  E("getrlimit64", Getrlimit);
  E("setrlimit", Setrlimit);
  E("setrlimit64", Setrlimit);
  E("sched_getcpu", SchedGetcpu);
  E("getpriority", Getpriority);
  E("setpriority", Setpriority);
  E("getauxval", Getauxval);
  E("__read_chk", ReadChk);
  E("__write_chk", WriteChk);
  E("__pread_chk", PreadChk);
  E("__pread64_chk", PreadChk);
  E("__pwrite_chk", PwriteChk);
  E("__pwrite64_chk", PwriteChk);
  E("__getcwd_chk", GetcwdChk);
  E("__readlink_chk", ReadlinkChk);
  E("__readlinkat_chk", ReadlinkatChk);
  E("__umask_chk", UmaskChk);
  E("stpcpy", Stpcpy);
  E("stpncpy", Stpncpy);
  E("__stpcpy_chk", StpcpyChk);
  E("__stpncpy_chk", StpncpyChk);
  E("memrchr", Memrchr);
  E("__memchr_chk", MemchrChk);
  E("__memrchr_chk", MemrchrChk);
  E("strsep", Strsep);
  E("strcasestr", Strcasestr);
  E("strchrnul", Strchrnul);
  E("strtok", Strtok);
  E("basename", Basename);
  E("__gnu_basename", Basename);
  E("dirname", Dirname);
#undef E
  // Export the live data object, not a snapshot of its array. Host startup
  // (including Java Os.setenv and the property session's _wputenv_s) can
  // resize/free that array after ELF relocations have bound this address.
  // The CRT keeps this slot current for both narrow and wide mutations.
  t["environ"] = &_environ;
  t["__environ"] = &_environ;
  t["__progname"] = &g_progname;
}

}  // namespace gesher::bionic
