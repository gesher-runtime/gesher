// gesher-base: <dirent.h> over FindFirstFileW / FindNextFileW, and over the
// virtual directories under /proc (procfs.cc).

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "internal.h"
#include "rooted_storage_client.h"
#include <gesher/reserved_names.h>

struct gesher_DIR {
  struct RootedScan {
    alignas(FILE_ID_BOTH_DIR_INFO) unsigned char bytes[16384]{};
    size_t offset = sizeof(bytes);
    bool restart = true;
    bool finished = false;
  };
  std::wstring path;     // the directory, as MapPath gave it
  std::wstring pattern;  // path\*
  HANDLE find = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAW data;
  bool have_data = false;  // `data` holds an entry not yet returned
  long long position = 0;
  bool is_virtual = false;
  std::unique_ptr<RootedScan> rooted;
  std::vector<gesher::procfs::Entry> entries;  // a virtual directory's
  int fd = -1;  // fdopendir's fd, or dirfd's; closed by closedir
  struct dirent entry;
};

namespace {

bool Start(DIR* dir) {
  dir->position = 0;
  if (dir->is_virtual) return gesher::procfs::List(dir->path, &dir->entries);
  if (dir->rooted) {
    dir->rooted->offset = sizeof(dir->rooted->bytes);
    dir->rooted->restart = true;
    dir->rooted->finished = false;
    return true;
  }
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

struct dirent* ReadRooted(DIR* dir) {
  auto& scan = *dir->rooted;
  if (scan.finished) return nullptr;
  if (scan.offset == sizeof(scan.bytes)) {
    const HANDLE directory = gesher::HandleFromFd(dir->fd);
    if (directory == INVALID_HANDLE_VALUE) return errno = EBADF, nullptr;
    const auto format = scan.restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
    if (!GetFileInformationByHandleEx(directory, format, scan.bytes, sizeof(scan.bytes))) {
      if (GetLastError() == ERROR_NO_MORE_FILES) scan.finished = true;
      else gesher::FailWithLastError();
      return nullptr;
    }
    scan.offset = 0;
    scan.restart = false;
  }
  const size_t available = sizeof(scan.bytes) - scan.offset;
  constexpr size_t kNameStart = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
  if (available < kNameStart || scan.offset % alignof(FILE_ID_BOTH_DIR_INFO) != 0) return errno = EIO, nullptr;
  auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(scan.bytes + scan.offset);
  const size_t used = item->NextEntryOffset != 0 ? item->NextEntryOffset : available;
  if (used > available || used < kNameStart || item->FileNameLength > used - kNameStart ||
      item->FileNameLength % sizeof(wchar_t) != 0) return errno = EIO, nullptr;
  scan.offset += used;
  const std::wstring name(item->FileName, item->FileNameLength / sizeof(wchar_t));
  const DWORD attrs = item->FileAttributes;
  const unsigned char type = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? DT_LNK :
      (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DT_DIR : DT_REG;
  Fill(&dir->entry, gesher::Narrow(gesher::reserved_names::Decoded(name.c_str()).c_str()), type,
       ++dir->position);
  if (item->FileId.QuadPart != 0) dir->entry.d_ino = static_cast<uint64_t>(item->FileId.QuadPart);
  return &dir->entry;
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
    const auto rooted = gesher::storage_root::Lookup(dir->path);
    if (rooted) {
      if (rooted.error != 0) {
        errno = rooted.error;
        delete dir;
        return nullptr;
      }
      dir->fd = fd >= 0 ? fd : open(name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dir->fd < 0) {
        delete dir;
        return nullptr;
      }
      dir->rooted = std::make_unique<gesher_DIR::RootedScan>();
      return dir;
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
  if (const auto scoped = gesher::storage_root::DescriptorPath(h); !scoped.empty()) {
    return Open(gesher::LinuxPath(scoped.c_str()).c_str(), fd);
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
  if (dir->rooted) return ReadRooted(dir);
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
  if (dir->is_virtual) return errno = ENOTSUP, -1;
  std::string path = gesher::Narrow(dir->path.c_str());
  dir->fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  return dir->fd;
}

}  // extern "C"
