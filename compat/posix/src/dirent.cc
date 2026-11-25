// gesher-base: <dirent.h> over FindFirstFileW / FindNextFileW, and over the
// virtual directories under /proc (procfs.cc).

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "internal.h"
#include "storage_policy.h"
#include "ofd_internal.h"
#include <gesher/file_description.h>
#include <gesher/procfs_fd.h>
#include <gesher/reserved_names.h>
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif

struct gesher_DIR {
  std::wstring path;     // the directory, as MapPath gave it
  std::wstring pattern;  // path\*
  HANDLE find = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW data;
  bool have_data = false;  // `data` holds an entry not yet returned
  long long position = 0;
  bool is_virtual = false;
  std::vector<gesher::procfs::Entry> entries;  // a virtual directory's
  int fd = -1;  // fdopendir's fd, or dirfd's; closed by closedir
  struct dirent entry;
};

namespace {

bool Start(DIR* dir) {
  dir->position = 0;
  if (dir->is_virtual) return gesher::procfs::List(dir->path, &dir->entries);
  dir->find = FindFirstFileExW(dir->pattern.c_str(), FindExInfoBasic, &dir->data,
                               FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  dir->have_data = dir->find != INVALID_HANDLE_VALUE;
  if (!dir->have_data && GetLastError() != ERROR_FILE_NOT_FOUND) {
    gesher::FailWithLastError();
    return false;
  }
  return true;
}

void Fill(struct dirent* e, const std::string& name, unsigned char type, long long position) {
  size_t len = name.size() < sizeof(e->d_name) - 1 ? name.size() : sizeof(e->d_name) - 1;
  memcpy(e->d_name, name.data(), len);
  e->d_name[len] = '\0';
  e->d_type = type;
  // Some code skips entries whose inode number is 0.
  e->d_ino = static_cast<unsigned long long>(position);
  e->d_off = position;
  e->d_reclen = sizeof(*e);
}

DIR* Open(const char* name, int fd) {
  if (name == nullptr || name[0] == '\0') {
    errno = name == nullptr ? EFAULT : ENOENT;
    return nullptr;
  }
  DIR* dir = new gesher_DIR();
  gesher::PathKind kind = gesher::MapPath(name, &dir->path);
  if (kind == gesher::PathKind::kVirtual) {
    dir->is_virtual = true;
  } else if (kind != gesher::PathKind::kFile) {
    errno = kind == gesher::PathKind::kMissing ? ENOENT : ENOTDIR;
    delete dir;
    return nullptr;
  } else {
    if (!gesher::CheckExternalStorageAccess(dir->path, gesher::AccessKind::kRead)) {
      delete dir;
      return nullptr;
    }
    DWORD attrs = GetFileAttributesW(dir->path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      gesher::FailWithLastError();
      delete dir;
      return nullptr;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      errno = ENOTDIR;
      delete dir;
      return nullptr;
    }
    dir->pattern = dir->path;
    if (!dir->pattern.empty() && dir->pattern.back() != L'\\' && dir->pattern.back() != L'/') {
      dir->pattern += L'\\';
    }
    dir->pattern += L'*';
  }
  if (!Start(dir)) {
    delete dir;
    return nullptr;
  }
  dir->fd = fd;
  return dir;
}

// The directory open as fd, as a Linux path: a /proc directory's own path
// (procfs.cc keeps it on the fd's file), or the directory on disk.
bool DirectoryOfFd(int fd, std::string* path) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, false;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) return errno = ENOTDIR, false;
  unsigned type;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0 &&
      gesher::procfs::DescribeTemporaryFile(h, &type, path)) {
    return type == S_IFDIR || (errno = ENOTDIR, false);
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return errno = ENOTDIR, false;
  wchar_t buffer[32768];
  DWORD n = GetFinalPathNameByHandleW(h, buffer, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) return gesher::FailWithLastError(), false;
  *path = gesher::LinuxPath(buffer);
  return true;
}

// getdents64's place in a directory, kept on the open file description as
// Linux keeps a directory's position there: an open enumeration (a find
// handle, or a /proc directory's entries) that each call continuing where the
// last one stopped reads on from. A call at a position the stream did not
// stop at (lseek, or a process sharing the description after fork) opens the
// directory again and skips to it. The stream goes with the description when
// its last fd closes.
struct DirectoryStream {
  // The description's f_pos_lock: calls on one description take turns, so
  // threads reading one fd share its entries out; calls on other fds do not
  // wait. Never held across fork by the child (see StreamOf).
  std::mutex lock;
  DWORD process = GetCurrentProcessId();
  DIR* dir = nullptr;
  std::string path;
  long long next = -1;  // the position the last call left the fd at
  // An entry read from `dir` that the last call's buffer could not hold.
  bool pending = false;
  std::string pending_name;
  unsigned char pending_type = 0;
  unsigned long long pending_ino = 0;

  void Close() {
    if (dir == nullptr) return;
    if (process != GetCurrentProcessId()) {
      // A forked child's copy: the find handle is the parent's, not this
      // process's, and must not be closed here (the value may name another
      // handle of the child's).
      dir->find = INVALID_HANDLE_VALUE;
      dir->fd = -1;
    }
    closedir(dir);
    dir = nullptr;
  }
  ~DirectoryStream() { Close(); }
};

// Taken only to find or attach a description's stream, never while waiting
// on anything else: a leaf in the fork set.
std::mutex g_attach_lock;

// The stream of fd's open file description, made on first use. A forked
// child does not use the parent's (its lock may have been held at fork, and
// its find handle is the parent's): it gets one of its own, which opens the
// directory again at the shared position.
std::shared_ptr<DirectoryStream> StreamOf(int fd) {
  std::shared_ptr<gesher::OpenFileDescription> description;
  if (!gesher::LookupOfd(fd, &description) || !description) {
    if (gesher::DescriptorSnapshotPtr snapshot = gesher::CaptureDescriptor(fd)) {
      description = snapshot->description;
    }
  }
  if (!description) return std::make_shared<DirectoryStream>();  // this call's only
  std::shared_ptr<DirectoryStream> stale;  // released after the lock
  std::lock_guard<std::mutex> guard(g_attach_lock);
  auto stream = std::static_pointer_cast<DirectoryStream>(description->directory_stream);
  if (!stream || stream->process != GetCurrentProcessId()) {
    stale = std::move(stream);
    stream = std::make_shared<DirectoryStream>();
    description->directory_stream = stream;
  }
  return stream;
}

}  // namespace

namespace gesher {

bool ProcfsDescriptor(int fd, std::string* linux_path, bool* directory) {
  HANDLE h = HandleFromFd(fd);
  unsigned type;
  if (h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK ||
      !procfs::DescribeHandle(h, &type, linux_path)) {
    return false;
  }
  *directory = type == S_IFDIR;
  return true;
}

int ProcfsFollow(const char* path, std::string* linux_path, int* fd, std::string* target) {
  std::wstring name;
  if (path == nullptr || MapPath(path, &name) != PathKind::kVirtual) return 0;
  const procfs::NameStatus status = procfs::Describe(name);
  if (status.type == 0) return 0;
  *linux_path = procfs::LinuxName(name);
  if (linux_path->rfind("/proc", 0) != 0 && linux_path->rfind("/sys", 0) != 0) return 0;
  *fd = -1;
  target->clear();
  if (status.type == S_IFLNK) {
    if (status.follow_error != 0) return errno = status.follow_error, -1;
    if (status.fd >= 0) {
      *fd = status.fd;
    } else if (!procfs::ReadLink(name, target)) {
      return -1;
    }
  }
  return 1;
}

}  // namespace gesher

extern "C" {

DIR* opendir(const char* name) { return Open(name, -1); }

// The directory open as fd, which the DIR then owns (closedir closes it).
DIR* fdopendir(int fd) {
  std::string path;
  if (!DirectoryOfFd(fd, &path)) return nullptr;
  return Open(path.c_str(), fd);
}

ssize_t getdents64(int fd, void* buffer, size_t size) {
  std::string path;
  if (!DirectoryOfFd(fd, &path)) return -1;
  if (buffer == nullptr) return errno = EFAULT, -1;
  std::shared_ptr<DirectoryStream> stream = StreamOf(fd);
  std::lock_guard<std::mutex> guard(stream->lock);
  // The position is the fd's (its open file description's, shared by dup
  // and fork), as Linux keeps a directory's.
  HANDLE h = gesher::HandleFromFd(fd);
  LARGE_INTEGER zero = {}, position = {};
  if (!SetFilePointerEx(h, zero, &position, FILE_CURRENT)) return gesher::FailWithLastError();
  long long index = position.QuadPart < 0 ? 0 : position.QuadPart;
  if (stream->dir == nullptr || index != stream->next || stream->path != path) {
    stream->Close();
    stream->pending = false;
    stream->next = -1;
    stream->dir = Open(path.c_str(), -1);
    if (stream->dir == nullptr) return -1;
    stream->path = path;
    long long skipped = 0;
    while (skipped < index && readdir(stream->dir) != nullptr) ++skipped;
    index = skipped;  // past the end: stays at the end
  }
  auto* out = static_cast<unsigned char*>(buffer);
  size_t used = 0;
  // struct linux_dirent64: d_ino (8), d_off (8), d_reclen (2), d_type (1),
  // d_name (NUL-terminated), the record padded to 8 bytes.
  constexpr size_t kNameOffset = 19;
  for (;;) {
    if (!stream->pending) {
      const int saved = errno;
      errno = 0;
      struct dirent* e = readdir(stream->dir);
      if (e == nullptr) {
        if (errno != 0 && used == 0) return -1;
        errno = saved;
        break;
      }
      errno = saved;
      stream->pending = true;
      stream->pending_name = e->d_name;
      stream->pending_type = e->d_type;
      stream->pending_ino = e->d_ino;
    }
    const size_t name_length = stream->pending_name.size();
    const size_t record = (kNameOffset + name_length + 1 + 7) & ~size_t{7};
    if (used + record > size) {
      // Kept: the next call starts with it.
      if (used == 0) return errno = EINVAL, -1;  // the buffer cannot hold one entry
      break;
    }
    stream->pending = false;
    ++index;
    unsigned char* r = out + used;
    memset(r, 0, record);
    const unsigned long long ino = stream->pending_ino;
    const long long next = index;
    const unsigned short reclen = static_cast<unsigned short>(record);
    memcpy(r, &ino, 8);
    memcpy(r + 8, &next, 8);
    memcpy(r + 16, &reclen, 2);
    r[18] = stream->pending_type;
    memcpy(r + kNameOffset, stream->pending_name.data(), name_length);
    used += record;
  }
  position.QuadPart = index;
  if (!SetFilePointerEx(h, position, nullptr, FILE_BEGIN)) {
    stream->next = -1;
    return gesher::FailWithLastError();
  }
  stream->next = index;
  return static_cast<ssize_t>(used);
}

struct dirent* readdir(DIR* dir) {
  struct dirent* e = &dir->entry;
  if (dir->is_virtual) {
    if (dir->position >= static_cast<long long>(dir->entries.size())) return nullptr;
    const gesher::procfs::Entry& v = dir->entries[dir->position];
    Fill(e, v.name, v.type, dir->position + 1);
    ++dir->position;
    return e;
  }
  if (!dir->have_data) {
    if (dir->find == INVALID_HANDLE_VALUE) return nullptr;
    if (!FindNextFileW(dir->find, &dir->data)) {
      if (GetLastError() != ERROR_NO_MORE_FILES) gesher::FailWithLastError();
      return nullptr;  // Only normal EOF leaves errno unchanged.
    }
  }
  dir->have_data = false;
  DWORD attrs = dir->data.dwFileAttributes;
  unsigned char type;
  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      (dir->data.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
       dir->data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
    type = DT_LNK;  // as lstat reports them (stat.cc)
  } else if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && dir->data.dwReserved0 == 0x80000024) {
    type = DT_FIFO;  // IO_REPARSE_TAG_LX_FIFO (fifo.cc)
  } else {
    type = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DT_DIR : DT_REG;
  }
  ++dir->position;
  // A name with a character Windows reserves is stored encoded.
  Fill(e, gesher::Narrow(gesher::reserved_names::Decoded(dir->data.cFileName).c_str()), type,
       dir->position);
  return e;
}

int readdir_r(DIR* dir, struct dirent* entry, struct dirent** result) {
  int saved = errno;
  errno = 0;
  struct dirent* e = readdir(dir);
  int error = errno;
  errno = saved;
  if (e == nullptr) {
    *result = nullptr;
    return error;
  }
  memcpy(entry, e, sizeof(*entry));
  *result = entry;
  return 0;
}

int closedir(DIR* dir) {
  if (dir == nullptr) return errno = EBADF, -1;
  if (dir->find != INVALID_HANDLE_VALUE) FindClose(dir->find);
  if (dir->fd >= 0) close(dir->fd);
  delete dir;
  return 0;
}

void rewinddir(DIR* dir) {
  if (dir->find != INVALID_HANDLE_VALUE) FindClose(dir->find);
  dir->find = INVALID_HANDLE_VALUE;
  Start(dir);
}

long telldir(DIR* dir) { return static_cast<long>(dir->position); }

void seekdir(DIR* dir, long position) {
  rewinddir(dir);
  while (dir->position < position && readdir(dir) != nullptr) {
  }
}

// An fd open on the directory, for the *at calls; closedir closes it.
int dirfd(DIR* dir) {
  if (dir->fd >= 0) return dir->fd;
  // A /proc directory's fd is one procfs.cc opens on its name.
  std::string path = dir->is_virtual ? gesher::procfs::LinuxName(dir->path)
                                     : gesher::Narrow(dir->path.c_str());
  dir->fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  return dir->fd;
}

}  // extern "C"

#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h): a leaf, held only to attach a stream. The streams'
// own locks stay out of the fork set: a child never takes the parent's
// (StreamOf gives it new streams), so one held at fork cannot block it, and a
// getdents64 running under the loader lock (a constructor) does not hold up
// fork by its whole scan.
GESHER_FORK_LOCK(gesher::kForkOrderLeaf, std::mutex, g_attach_lock);
#endif
