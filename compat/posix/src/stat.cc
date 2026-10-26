// gesher-base: stat, fstat and lstat with Linux's struct stat
// (<sys/stat.h>), over Win32 file information. Defined under the CRT's names,
// like close and read in fd.cc. fstatat is in path.cc.
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

extern "C" int gesher_is_socket(int fd);

namespace {

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr long long kEpochDifference = 116444736000000000LL;

timespec ToTimespec(LARGE_INTEGER t) {
  long long since_1970 = t.QuadPart - kEpochDifference;
  long long seconds = since_1970 / 10000000;
  long long rest = since_1970 % 10000000;
  if (rest < 0) {  // before 1970: keep tv_nsec in [0, 1e9)
    rest += 10000000;
    --seconds;
  }
  timespec ts;
  ts.tv_sec = static_cast<time_t>(seconds);
  ts.tv_nsec = static_cast<long>(rest * 100);
  return ts;
}

void Clear(struct stat* buf) {
  memset(buf, 0, sizeof(*buf));
  buf->st_uid = getuid();
  buf->st_gid = getgid();
  buf->st_nlink = 1;
  buf->st_blksize = 4096;
}

// A file or directory on disk.
int FromHandle(HANDLE h, struct stat* buf) {
  BY_HANDLE_FILE_INFORMATION info;
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
  if (!GetFileInformationByHandle(h, &info) ||
      !GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic)) ||
      !GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof(standard))) {
    return gesher::FailWithLastError();
  }
  Clear(buf);
  buf->st_dev = info.dwVolumeSerialNumber;
  buf->st_ino = (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  buf->st_nlink = info.nNumberOfLinks;
  // Unlinked while open, where the delete is not POSIX-style (the volume or
  // Windows version lacks it): the name stays until the last handle closes,
  // but Linux's count has already dropped.
  if (standard.DeletePending && buf->st_nlink > 0) buf->st_nlink--;
  // Permissions are not modelled beyond the read-only attribute. Windows has
  // no execute permission and access(X_OK) holds for every file (fsops.cc:
  // apps check File.canExecute() before starting their ELF programs), so
  // regular files show the execute bits too, as an NTFS volume mounted on
  // Linux does (ADR 0057).
  bool read_only = (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    buf->st_mode = S_IFDIR | 0755;
  } else {
    buf->st_mode = S_IFREG | (read_only ? 0555 : 0755);
    buf->st_size = standard.EndOfFile.QuadPart;
  }
  buf->st_blocks = (standard.AllocationSize.QuadPart + 511) / 512;
  buf->st_atim = ToTimespec(basic.LastAccessTime);
  buf->st_mtim = ToTimespec(basic.LastWriteTime);
  buf->st_ctim = ToTimespec(basic.ChangeTime);
  return 0;
}

}  // namespace

namespace {

// stat and lstat: `follow` follows a symbolic link at the end of the path.
int StatPath(const char* path, struct stat* buf, bool follow, int depth = 0) {
  if (path == nullptr || buf == nullptr) return errno = EFAULT, -1;
  if (path[0] == '\0') return errno = ENOENT, -1;
  std::wstring windows;
  switch (gesher::MapPath(path, &windows)) {
    case gesher::PathKind::kFile: {
      // Directories need backup semantics to be opened; no access right is
      // needed to read attributes.
      HANDLE h = CreateFileW(windows.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT),
                             nullptr);
      if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
      int result = FromHandle(h, buf);
      if (result == 0 && !follow && gesher::IsSymlinkHandle(h)) {
        // A symbolic link (or a junction): Linux reports the link, its size
        // the length of its target.
        std::string target;
        buf->st_mode = S_IFLNK | 0777;
        buf->st_size = gesher::ReadSymlinkHandle(h, &target) ? static_cast<long long>(target.size()) : 0;
      }
      CloseHandle(h);
      return result;
    }
    case gesher::PathKind::kVirtual: {
      unsigned type = gesher::procfs::Type(windows);
      if (type == 0) break;
      if (type == S_IFLNK && follow) {
        // /proc/self/fd/<n> is the file open as fd n, a pipe or socket too.
        int fd = gesher::procfs::FdOfLink(windows);
        if (fd >= 0) return fstat(fd, buf);
        std::string target;
        if (depth > 8) return errno = ELOOP, -1;
        if (!gesher::procfs::ReadLink(windows, &target)) return -1;
        return StatPath(target.c_str(), buf, true, depth + 1);
      }
      // Size 0, like Linux's /proc files.
      Clear(buf);
      buf->st_mode = type | (type == S_IFDIR ? 0555 : type == S_IFLNK ? 0777 : 0444);
      buf->st_dev = 0x16;  // a file system of its own, as procfs is
      return 0;
    }
    case gesher::PathKind::kNull:
    case gesher::PathKind::kZero:
    case gesher::PathKind::kRandom:
    case gesher::PathKind::kBinder:
      Clear(buf);
      buf->st_mode = S_IFCHR | 0666;
      return 0;
    case gesher::PathKind::kMissing:
      break;
  }
  return errno = ENOENT, -1;
}

}  // namespace

extern "C" {

int stat(const char* path, struct stat* buf) { return StatPath(path, buf, true); }

int lstat(const char* path, struct stat* buf) { return StatPath(path, buf, false); }

int fstat(int fd, struct stat* buf) {
  if (buf == nullptr) return errno = EFAULT, -1;
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if (gesher_is_socket(fd)) {
    Clear(buf);
    buf->st_mode = S_IFSOCK | 0777;
    return 0;
  }
  switch (GetFileType(h)) {
    case FILE_TYPE_DISK:
      return FromHandle(h, buf);
    case FILE_TYPE_PIPE:
      Clear(buf);
      buf->st_mode = S_IFIFO | 0600;
      return 0;
    default:  // consoles, NUL and the devices on it
      Clear(buf);
      buf->st_mode = S_IFCHR | 0666;
      return 0;
  }
}

}  // extern "C"
