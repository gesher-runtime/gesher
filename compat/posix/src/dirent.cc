// gesher-base: <dirent.h> over FindFirstFileW / FindNextFileW, and over the
// virtual directories under /proc (procfs.cc).

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "internal.h"
#include "storage_policy.h"
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
      gesher::procfs::DescribeHandle(h, &type, path)) {
    return type == S_IFDIR || (errno = ENOTDIR, false);
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return errno = ENOTDIR, false;
  wchar_t buffer[32768];
  DWORD n = GetFinalPathNameByHandleW(h, buffer, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) return gesher::FailWithLastError(), false;
  *path = gesher::LinuxPath(buffer);
  return true;
}

// getdents64's listing of a directory, kept per open file description as
// Linux keeps a directory's position there: one enumeration serves every
// call that continues where the last one stopped (no re-reading the
// directory up to the position on each call). A call at position 0
// (rewind), or at a position this listing did not stop at (lseek, or
// another process sharing the description after fork), enumerates afresh.
struct Listed {
  std::string name;
  unsigned char type;
  unsigned long long ino;
};
struct DirectoryStream {
  std::weak_ptr<gesher::OpenFileDescription> description;
  std::string path;
  std::vector<Listed> entries;
  long long next = -1;  // the position the last call left the fd at
};
// One lock serializes getdents64 calls, as Linux's f_pos_lock does those on
// one description: two threads reading one fd share its entries out between
// them. Taken first at fork (nothing holds another lock when it is taken).
std::mutex g_streams_lock;
std::map<const void*, DirectoryStream> g_streams;

// The directory's entries, as readdir gives them.
bool ListDirectory(const std::string& path, std::vector<Listed>* entries) {
  DIR* dir = Open(path.c_str(), -1);
  if (dir == nullptr) return false;
  entries->clear();
  int saved = errno;
  errno = 0;
  while (struct dirent* e = readdir(dir)) entries->push_back({e->d_name, e->d_type, e->d_ino});
  const int error = errno;
  closedir(dir);
  if (error != 0) return errno = error, false;
  errno = saved;
  return true;
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
  std::lock_guard<std::mutex> guard(g_streams_lock);
  // The position is the fd's (its open file description's, shared by dup
  // and fork), as Linux keeps a directory's.
  HANDLE h = gesher::HandleFromFd(fd);
  LARGE_INTEGER zero = {}, position = {};
  if (!SetFilePointerEx(h, zero, &position, FILE_CURRENT)) return gesher::FailWithLastError();
  // Listings of closed descriptions go.
  for (auto it = g_streams.begin(); it != g_streams.end();) {
    it = it->second.description.expired() ? g_streams.erase(it) : std::next(it);
  }
  DirectoryStream local;
  DirectoryStream* stream = &local;
  if (gesher::DescriptorSnapshotPtr snapshot = gesher::CaptureDescriptor(fd)) {
    if (snapshot->description) {
      stream = &g_streams[snapshot->description.get()];
      if (stream->description.lock() != snapshot->description) {
        *stream = DirectoryStream();
        stream->description = snapshot->description;
      }
    }
  }
  if (position.QuadPart == 0 || position.QuadPart != stream->next || stream->path != path) {
    stream->next = -1;
    stream->path = path;
    if (!ListDirectory(path, &stream->entries)) {
      stream->entries.clear();
      return -1;
    }
  }
  auto* out = static_cast<unsigned char*>(buffer);
  size_t used = 0;
  long long index = position.QuadPart < 0 ? 0 : position.QuadPart;
  // struct linux_dirent64: d_ino (8), d_off (8), d_reclen (2), d_type (1),
  // d_name (NUL-terminated), the record padded to 8 bytes.
  constexpr size_t kNameOffset = 19;
  for (; index < static_cast<long long>(stream->entries.size()); ++index) {
    const Listed& e = stream->entries[static_cast<size_t>(index)];
    const size_t name_length = e.name.size();
    const size_t record = (kNameOffset + name_length + 1 + 7) & ~size_t{7};
    if (used + record > size) {
      // Not taken: the next call (from `index`) starts with it.
      if (used == 0) return errno = EINVAL, -1;  // the buffer cannot hold one entry
      break;
    }
    unsigned char* r = out + used;
    memset(r, 0, record);
    const unsigned long long ino = e.ino;
    const long long next = index + 1;
    const unsigned short reclen = static_cast<unsigned short>(record);
    memcpy(r, &ino, 8);
    memcpy(r + 8, &next, 8);
    memcpy(r + 16, &reclen, 2);
    r[18] = e.type;
    memcpy(r + kNameOffset, e.name.data(), name_length);
    used += record;
  }
  if (index > static_cast<long long>(stream->entries.size())) {
    index = static_cast<long long>(stream->entries.size());  // past the end: stays there
  }
  position.QuadPart = index;
  if (!SetFilePointerEx(h, position, nullptr, FILE_BEGIN)) return gesher::FailWithLastError();
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
// fork (gesher/fork.h): before every other lock, since getdents64 takes
// descriptor and loader locks while it holds it.
GESHER_FORK_LOCK(gesher::kForkOrderLoader - 10, std::mutex, g_streams_lock);
#endif
