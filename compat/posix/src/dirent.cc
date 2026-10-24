// gesher-base: <dirent.h> over FindFirstFileW / FindNextFileW, and over the
// virtual directories under /proc (procfs.cc).

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <string>
#include <vector>

#include "internal.h"

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

}  // namespace

extern "C" {

DIR* opendir(const char* name) { return Open(name, -1); }

// The directory open as fd, which the DIR then owns (closedir closes it).
DIR* fdopendir(int fd) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return nullptr;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    errno = ENOTDIR;
    return nullptr;
  }
  wchar_t path[32768];
  DWORD n = GetFinalPathNameByHandleW(h, path, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) {
    gesher::FailWithLastError();
    return nullptr;
  }
  return Open(gesher::LinuxPath(path).c_str(), fd);
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
    if (dir->find == INVALID_HANDLE_VALUE || !FindNextFileW(dir->find, &dir->data)) {
      return nullptr;  // end of directory; errno unchanged
    }
  }
  dir->have_data = false;
  DWORD attrs = dir->data.dwFileAttributes;
  unsigned char type;
  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      (dir->data.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
       dir->data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
    type = DT_LNK;  // as lstat reports them (stat.cc)
  } else {
    type = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DT_DIR : DT_REG;
  }
  ++dir->position;
  Fill(e, gesher::Narrow(dir->data.cFileName), type, dir->position);
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
  if (dir->is_virtual) return errno = ENOTSUP, -1;
  std::string path = gesher::Narrow(dir->path.c_str());
  dir->fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  return dir->fd;
}

}  // extern "C"
