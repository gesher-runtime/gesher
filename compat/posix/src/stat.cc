// gesher-base: stat, fstat and lstat with Linux's struct stat
// (<sys/stat.h>), over Win32 file information. Defined under the CRT's names,
// like close and read in fd.cc. fstatat is in path.cc.
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fifo_internal.h"
#include "internal.h"
#include "ofd_internal.h"
#include "revocable_internal.h"
#include "usb_block_internal.h"
#include "storage_policy.h"

extern "C" int gesher_is_socket(int fd);

namespace {

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr long long kEpochDifference = 116444736000000000LL;

// The device of /proc's names (a file system of its own, as procfs is), for
// stat of a name and fstat of an fd open on one alike.
constexpr unsigned long long kProcDevice = 0x16;

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

// A /proc or /sys name: size 0, like Linux's; procfs's device, the name's
// inode, and its times (procfs.cc), the same for stat of the name and fstat
// of an fd open on it.
void ProcName(const std::string& linux_path, unsigned type, unsigned mode, struct stat* buf) {
  Clear(buf);
  buf->st_mode = type | mode;
  buf->st_dev = kProcDevice;
  buf->st_ino = gesher::procfs::Inode(linux_path);
  LARGE_INTEGER time;
  time.QuadPart = gesher::procfs::Time(linux_path);
  buf->st_atim = buf->st_mtim = buf->st_ctim = ToTimespec(time);
}

// An fd open on a /proc (or /sys) name (an anonymous, temporary file that
// procfs.cc marked): what stat of the name reports (lstat for a link opened
// O_PATH | O_NOFOLLOW), not the snapshot file. `description` caches whether
// the file is one, so a memfd's fstat reads the attribute once only.
bool ProcFile(HANDLE h, gesher::OpenFileDescription* description, struct stat* buf) {
  if (description != nullptr && description->procfs_file.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  unsigned type, mode;
  std::string path;
  const bool tagged = gesher::procfs::DescribeTemporaryFile(h, &type, &path, &mode);
  if (description != nullptr) description->procfs_file.store(tagged ? 1 : 0, std::memory_order_relaxed);
  if (tagged) ProcName(path, type, mode, buf);
  return tagged;
}

// A file or directory on disk.
int FromHandle(HANDLE h, struct stat* buf, gesher::OpenFileDescription* description = nullptr) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) return gesher::FailWithLastError();
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0 && ProcFile(h, description, buf)) {
    return 0;
  }
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
  if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic)) ||
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
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && gesher::IsSymlinkHandle(h)) {
    // A handle on a symbolic link (or junction) itself, lstat's or an fd
    // opened O_PATH | O_NOFOLLOW: Linux reports the link, its size the
    // length of its target.
    std::string target;
    buf->st_mode = S_IFLNK | 0777;
    buf->st_size = gesher::ReadSymlinkHandle(h, &target) ? static_cast<long long>(target.size()) : 0;
  } else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    buf->st_mode = S_IFDIR | 0755;
  } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && gesher::IsFifoHandle(h)) {
    // A FIFO (fifo.cc): no data of its own, and nothing to execute.
    buf->st_mode = S_IFIFO | (read_only ? 0444 : 0666);
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
      if (!gesher::CheckExternalStorageAccess(windows, gesher::AccessKind::kStat)) return -1;
      // Directories need backup semantics to be opened; no access right is
      // needed to read attributes.
      HANDLE h = gesher::CreateFileFollowingFifo(windows.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT));
      if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
      int result = gesher::CheckExternalStorageHandle(h, gesher::AccessKind::kStat)
                       ? FromHandle(h, buf) : -1;
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
      // One lookup for the name (a map_files entry's costs a VirtualQuery).
      const gesher::procfs::NameStatus name = gesher::procfs::Describe(windows);
      if (name.type == 0) break;
      if (name.type == S_IFBLK) return gesher::usb_block::StatNode(
          gesher::Narrow(windows.substr(windows.find_last_of(L'/') + 1).c_str()), buf);
      if (name.type == S_IFDIR && !follow && windows == L"proc/self") {
        // The link itself, as an fd opened on it O_PATH | O_NOFOLLOW is.
        ProcName("/proc/self", S_IFLNK, 0777, buf);
        return 0;
      }
      if (name.type == S_IFLNK && follow) {
        if (name.follow_error != 0) return errno = name.follow_error, -1;
        // /proc/self/fd/<n> is the file open as fd n, a pipe or socket too.
        if (name.fd >= 0) return fstat(name.fd, buf);
        std::string target;
        if (depth > 8) return errno = ELOOP, -1;
        if (!gesher::procfs::ReadLink(windows, &target)) return -1;
        return StatPath(target.c_str(), buf, true, depth + 1);
      }
      ProcName(gesher::procfs::LinuxName(windows), name.type, name.mode, buf);
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
  if (gesher_is_socket(fd)) {
    Clear(buf);
    buf->st_mode = S_IFSOCK | 0777;
    return 0;
  }
  auto snapshot = gesher::CaptureDescriptor(fd);
  if (!snapshot) return -1;
  if (snapshot->description && !snapshot->description->virtual_directory.empty()) {
    if (gesher::procfs::Type(snapshot->description->virtual_directory) != S_IFDIR) return errno = ENOENT, -1;
    ProcName(gesher::procfs::LinuxName(snapshot->description->virtual_directory), S_IFDIR, 0555, buf);
    return 0;
  }
  int block_result;
  if (gesher::usb_block::StatFd(snapshot, buf, &block_result)) return block_result;
  gesher::FileOperationLease operation(snapshot);
  if (operation.Error()) return errno = operation.Error(), -1;
  const HANDLE h = snapshot->handle;
  if (snapshot->kind == gesher::DescriptorKind::kFifo) {
    gesher::FifoFileStatus status;
    if (gesher::FifoStatus(snapshot, &status) < 0) return -1;
    Clear(buf);
    buf->st_mode = S_IFIFO | status.mode;
    buf->st_dev = status.device;
    buf->st_ino = status.inode;
    LARGE_INTEGER time;
    time.QuadPart = status.access_time;
    buf->st_atim = ToTimespec(time);
    time.QuadPart = status.write_time;
    buf->st_mtim = ToTimespec(time);
    time.QuadPart = status.change_time;
    buf->st_ctim = ToTimespec(time);
    return 0;
  }
  switch (snapshot->kind == gesher::DescriptorKind::kUnshareable ? FILE_TYPE_CHAR : GetFileType(h)) {
    case FILE_TYPE_DISK:
      return FromHandle(h, buf, snapshot->description.get());
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
