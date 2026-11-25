// gesher-base: <dirent.h> over FindFirstFileW / FindNextFileW, and over the
// virtual directories under /proc (procfs.cc).

#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "fifo_internal.h"
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

// The directory `h` (an fd's handle) is open on, as a Linux path: a /proc
// directory's own path (procfs.cc keeps it on the fd's file), or the
// directory on disk; *on_disk tells which.
bool DirectoryOfHandle(HANDLE h, std::string* path, bool* on_disk) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) return errno = ENOTDIR, false;
  unsigned type;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0 &&
      gesher::procfs::DescribeTemporaryFile(h, &type, path)) {
    *on_disk = false;
    return type == S_IFDIR || (errno = ENOTDIR, false);
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return errno = ENOTDIR, false;
  *on_disk = true;
  std::wstring buffer(32768, L'\0');
  DWORD n = GetFinalPathNameByHandleW(h, buffer.data(), 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) {
    // A directory removed while open has no name left: still a directory.
    FILE_STANDARD_INFO standard;
    if (GetFileInformationByHandleEx(h, FileStandardInfo, &standard, sizeof(standard)) &&
        standard.DeletePending) {
      path->clear();
      return true;
    }
    return gesher::FailWithLastError(), false;
  }
  buffer.resize(n);
  *path = gesher::LinuxPath(buffer.c_str());
  return true;
}

bool DirectoryOfFd(int fd, std::string* path) {
  HANDLE h = gesher::HandleFromFd(fd);
  if (h == INVALID_HANDLE_VALUE) return errno = EBADF, false;
  bool on_disk;
  return DirectoryOfHandle(h, path, &on_disk);
}

// A directory entry's d_type, from its attributes and reparse tag.
unsigned char EntryType(DWORD attributes, DWORD reparse_tag) {
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      (reparse_tag == IO_REPARSE_TAG_SYMLINK || reparse_tag == IO_REPARSE_TAG_MOUNT_POINT)) {
    return DT_LNK;  // as lstat reports them (stat.cc)
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && reparse_tag == 0x80000024) {
    return DT_FIFO;  // IO_REPARSE_TAG_LX_FIFO (fifo.cc)
  }
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? DT_DIR : DT_REG;
}

// --- getdents64's streams ------------------------------------------------------------
//
// A stream is getdents64's place in a directory, kept on the open file
// description as Linux keeps a directory's position there. It reads from a
// source: on disk a directory handle of its own (the fd's directory, opened
// by its file ID) enumerated with GetFileInformationByHandleEx; for /proc a
// DIR of the directory's entries (no handles). A call that continues where
// the last one stopped reads on; a call at a position the stream did not stop
// at (lseek, or a process sharing the description after fork) makes a new
// source, enumerated from the start and skipped to it: positions are
// ordinals. Sources are made outside every lock and swapped in.
//
// Fork: nothing of a stream is in the fork set, so a fork never waits for a
// getdents64 (a slow volume, a long skip, a /proc snapshot that takes the
// mman or fd-table locks). The directory handles are listed in a registry
// under g_attach_lock, a leaf held for a few instructions; a forked child
// closes the ones it inherited and starts a new generation: a stream of an
// older one is replaced on its next use, and never touched or freed (its
// state and lock may be a parent's thread's, mid-call).

// Taken only to attach a stream or (un)register a handle, never while
// waiting on anything else: a leaf in the fork set.
std::mutex g_attach_lock;
std::vector<HANDLE>* g_stream_handles = nullptr;  // under g_attach_lock
std::atomic<unsigned> g_generation{0};

void RegisterHandle(HANDLE h) {
  std::lock_guard<std::mutex> guard(g_attach_lock);
  if (g_stream_handles == nullptr) g_stream_handles = new std::vector<HANDLE>();
  g_stream_handles->push_back(h);
}

// Closed under the lock, so that a fork sees it either registered and open,
// or neither.
void CloseRegisteredHandle(HANDLE h) {
  std::lock_guard<std::mutex> guard(g_attach_lock);
  if (g_stream_handles != nullptr) {
    for (size_t i = 0; i < g_stream_handles->size(); ++i) {
      if ((*g_stream_handles)[i] == h) {
        (*g_stream_handles)[i] = g_stream_handles->back();
        g_stream_handles->pop_back();
        break;
      }
    }
  }
  CloseHandle(h);
}

constexpr size_t kFirstBatch = 4 * 1024;  // a small directory's whole listing
constexpr size_t kBatch = 64 * 1024;

struct Source {
  std::string path;
  long long position = 0;  // entries read so far
  HANDLE handle = nullptr;  // on disk, registered
  bool restart = true;
  bool ended = false;
  std::vector<unsigned char> batch;  // FILE_ID_BOTH_DIR_INFO records
  size_t batch_offset = 0;
  bool batch_valid = false;
  DIR* dir = nullptr;  // /proc

  ~Source() {
    if (handle != nullptr) CloseRegisteredHandle(handle);
    if (dir != nullptr) closedir(dir);
  }

  // The next entry (its d_ino: the file ID on disk, 0 when there is none),
  // or false at the end (errno 0) or on an error (errno set).
  bool Read(std::string* name, unsigned char* type, unsigned long long* ino) {
    if (dir != nullptr) {
      errno = 0;
      struct dirent* e = readdir(dir);
      if (e == nullptr) return false;
      *name = e->d_name;
      *type = e->d_type;
      *ino = 0;
      ++position;
      return true;
    }
    if (!batch_valid) {
      if (ended || handle == nullptr) return errno = 0, false;
      batch.resize(restart ? kFirstBatch : kBatch);
      if (!GetFileInformationByHandleEx(handle,
                                        restart ? FileIdBothDirectoryRestartInfo
                                                : FileIdBothDirectoryInfo,
                                        batch.data(), static_cast<DWORD>(batch.size()))) {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND) {
          // The listing is over: its buffer goes now, not with the fd.
          ended = true;
          std::vector<unsigned char>().swap(batch);
          return errno = 0, false;
        }
        gesher::FailWithLastError();
        return false;
      }
      restart = false;
      batch_valid = true;
      batch_offset = 0;
    }
    const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(batch.data() + batch_offset);
    if (info->NextEntryOffset == 0) {
      batch_valid = false;
    } else {
      batch_offset += info->NextEntryOffset;
    }
    const std::wstring wide(info->FileName, info->FileNameLength / sizeof(wchar_t));
    // A name with a character Windows reserves is stored encoded.
    *name = gesher::Narrow(gesher::reserved_names::Decoded(wide.c_str()).c_str());
    // EaSize holds the reparse tag of a reparse point.
    *type = EntryType(info->FileAttributes, info->EaSize);
    *ino = static_cast<unsigned long long>(info->FileId.QuadPart);
    ++position;
    return true;
  }
};

// The fd's directory again, for a new source: by its file ID (the very
// directory, whatever its name is now), else by its name, checked to be the
// same directory. Not inheritable. nullptr with *gone when it has been
// removed (an empty listing, as Linux's for a removed directory).
HANDLE OpenDirectory(HANDLE fd_handle, bool* gone) {
  *gone = false;
  constexpr DWORD kAccess = FILE_LIST_DIRECTORY | SYNCHRONIZE;
  constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  BY_HANDLE_FILE_INFORMATION fd_info;
  if (!GetFileInformationByHandle(fd_handle, &fd_info)) return gesher::FailWithLastError(), nullptr;
  FILE_STANDARD_INFO standard;
  if (GetFileInformationByHandleEx(fd_handle, FileStandardInfo, &standard, sizeof(standard)) &&
      standard.DeletePending) {
    *gone = true;
    return nullptr;
  }
  FILE_ID_DESCRIPTOR id = {};
  id.dwSize = sizeof(id);
  id.Type = FileIdType;
  id.FileId.QuadPart = static_cast<LONGLONG>((static_cast<unsigned long long>(fd_info.nFileIndexHigh) << 32) |
                                             fd_info.nFileIndexLow);
  HANDLE h = OpenFileById(fd_handle, &id, kAccess, kShare, nullptr, FILE_FLAG_BACKUP_SEMANTICS);
  if (h != INVALID_HANDLE_VALUE) return h;
  std::wstring windows(32768, L'\0');
  const DWORD n = GetFinalPathNameByHandleW(fd_handle, windows.data(), 32768,
                                            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n == 0 || n >= 32768) return gesher::FailWithLastError(), nullptr;
  windows.resize(n);
  h = CreateFileW(windows.c_str(), kAccess, kShare, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                  nullptr);
  if (h == INVALID_HANDLE_VALUE) return gesher::FailWithLastError(), nullptr;
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info) ||
      info.dwVolumeSerialNumber != fd_info.dwVolumeSerialNumber ||
      info.nFileIndexHigh != fd_info.nFileIndexHigh || info.nFileIndexLow != fd_info.nFileIndexLow) {
    // Renamed and replaced between the two calls: not the fd's directory.
    CloseHandle(h);
    return errno = ESTALE, nullptr;
  }
  return h;
}

// A new source for the fd's directory, at `position` (fewer when the listing
// is shorter). Runs under no lock.
std::unique_ptr<Source> MakeSource(HANDLE fd_handle, bool on_disk, const std::string& path,
                                   long long position) {
  auto source = std::make_unique<Source>();
  source->path = path;
  if (!on_disk) {
    source->dir = Open(path.c_str(), -1);
    if (source->dir == nullptr) return nullptr;
  } else {
    bool gone;
    HANDLE h = OpenDirectory(fd_handle, &gone);
    if (h == nullptr && !gone) return nullptr;
    if (h != nullptr) {
      RegisterHandle(h);
      source->handle = h;
    } else {
      source->ended = true;  // removed: nothing to list
    }
  }
  std::string name;
  unsigned char type;
  unsigned long long ino;
  while (source->position < position && source->Read(&name, &type, &ino)) {
  }
  if (errno != 0 && source->position < position) return nullptr;
  return source;
}

struct DirectoryStream {
  // The description's f_pos_lock: calls on one description take turns, so
  // threads reading one fd share its entries out; calls on other fds do not
  // wait. Held only while reading the source and moving the position.
  std::mutex lock;
  unsigned generation = g_generation.load();
  std::unique_ptr<Source> source;
  long long next = -1;  // the position the last call left the fd at
  // An entry read that the last call's buffer could not hold.
  bool pending = false;
  std::string pending_name;
  unsigned char pending_type = 0;
  unsigned long long pending_ino = 0;
};

// A stream of an older generation (a parent's, in a forked child) is never
// freed: its memory may be in the middle of a parent's thread's change.
void DeleteStream(DirectoryStream* stream) {
  if (stream->generation == g_generation.load()) delete stream;
}

// The stream of `description`, made on first use (or anew in a forked
// child); without a description, one for this call only.
std::shared_ptr<DirectoryStream> StreamOf(const std::shared_ptr<gesher::OpenFileDescription>& description) {
  if (!description) return std::shared_ptr<DirectoryStream>(new DirectoryStream(), DeleteStream);
  std::shared_ptr<void> replaced;  // dropped after the lock
  std::lock_guard<std::mutex> guard(g_attach_lock);
  auto stream = std::static_pointer_cast<DirectoryStream>(description->directory_stream);
  if (!stream || stream->generation != g_generation.load()) {
    replaced = std::move(description->directory_stream);
    stream = std::shared_ptr<DirectoryStream>(new DirectoryStream(), DeleteStream);
    description->directory_stream = stream;
  }
  return stream;
}

#ifdef GESHER_FORK_LOCK
// In a forked child, which holds g_attach_lock and runs alone: the directory
// handles it inherited are closed (the parent's streams' are no use to it,
// and a program it starts must not get them), and a new generation begins.
void AttachForkChild(void* lock) {
  if (g_stream_handles != nullptr) {
    for (HANDLE h : *g_stream_handles) {
      if (gesher::InheritedAtFork(h)) CloseHandle(h);
    }
    g_stream_handles->clear();
  }
  g_generation.fetch_add(1);
  new (lock) std::mutex();
}
bool AttachTryLock(void* lock) { return static_cast<std::mutex*>(lock)->try_lock(); }
void AttachUnlock(void* lock) { static_cast<std::mutex*>(lock)->unlock(); }
#endif

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

bool FifoFilePath(int fd, std::wstring* windows_path, bool* is_fifo) {
  *is_fifo = false;
  if (GetDescriptorKind(fd) != DescriptorKind::kFifo) return true;
  DescriptorSnapshotPtr snapshot = CaptureDescriptor(fd);
  if (!snapshot || snapshot->kind != DescriptorKind::kFifo) return true;
  *is_fifo = true;
  HANDLE file = OpenFifoFile(snapshot, FILE_READ_ATTRIBUTES);
  if (file == INVALID_HANDLE_VALUE || file == nullptr) {
    // Renamed or removed since it was opened: its file system is still the
    // volume it was on (Linux keeps the inode): found by the volume serial
    // fstat reports for it.
    FifoFileStatus status;
    if (FifoStatus(snapshot, &status) < 0) return false;
    wchar_t drives[512];
    const DWORD length = GetLogicalDriveStringsW(512, drives);
    for (const wchar_t* d = drives; length != 0 && length < 512 && *d != L'\0'; d += wcslen(d) + 1) {
      DWORD serial = 0;
      if (GetVolumeInformationW(d, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) &&
          serial == static_cast<DWORD>(status.device)) {
        windows_path->assign(d);
        return true;
      }
    }
    return errno = EOPNOTSUPP, false;
  }
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetFinalPathNameByHandleW(file, buffer.data(), 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  CloseHandle(file);
  if (n == 0 || n >= 32768) return FailWithLastError(), false;
  buffer.resize(n);
  *windows_path = std::move(buffer);
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
  // The fd is resolved once: another thread's close and open on its number
  // cannot turn this call to another file (Linux pins it with fdget_pos).
  gesher::DescriptorSnapshotPtr snapshot = gesher::CaptureDescriptor(fd);
  if (!snapshot) return errno == 0 ? (errno = EBADF, -1) : -1;
  const HANDLE h = snapshot->handle;
  if (GetFileType(h) != FILE_TYPE_DISK) return errno = ENOTDIR, -1;
  std::string path;
  bool on_disk;
  if (!DirectoryOfHandle(h, &path, &on_disk)) return -1;
  if (buffer == nullptr) return errno = EFAULT, -1;
  std::shared_ptr<DirectoryStream> stream = StreamOf(snapshot->description);
  LARGE_INTEGER zero = {}, position = {};
  long long read_at = 0;  // the fd's position as this call found it
  std::unique_lock<std::mutex> guard(stream->lock);
  // The position is the description's (shared by dup and fork), as Linux
  // keeps a directory's.
  for (int attempt = 0;; ++attempt) {
    if (!SetFilePointerEx(h, zero, &position, FILE_CURRENT)) return gesher::FailWithLastError();
    read_at = position.QuadPart;
    if (position.QuadPart < 0) position.QuadPart = 0;
    if (stream->source && position.QuadPart == stream->next && stream->source->path == path) break;
    // A new source, made with the lock released (it may take long, and a
    // /proc snapshot takes other locks), then swapped in if the position is
    // still the one it was made for.
    guard.unlock();
    std::unique_ptr<Source> made = MakeSource(h, on_disk, path, position.QuadPart);
    guard.lock();
    if (!made) return -1;
    LARGE_INTEGER now = {};
    if (!SetFilePointerEx(h, zero, &now, FILE_CURRENT)) return gesher::FailWithLastError();
    if (now.QuadPart == read_at || attempt >= 3) {
      position.QuadPart = made->position;  // past the end: stays at the end
      std::swap(stream->source, made);
      stream->pending = false;
      stream->next = position.QuadPart;
      guard.unlock();
      made.reset();  // the old source, closed with no lock held
      guard.lock();
      if (stream->next == position.QuadPart && stream->source &&
          stream->source->path == path) {
        break;
      }
    }
  }
  Source* source = stream->source.get();
  long long index = position.QuadPart;
  auto* out = static_cast<unsigned char*>(buffer);
  size_t used = 0;
  // struct linux_dirent64: d_ino (8), d_off (8), d_reclen (2), d_type (1),
  // d_name (NUL-terminated), the record padded to 8 bytes.
  constexpr size_t kNameOffset = 19;
  for (;;) {
    if (!stream->pending) {
      const int saved = errno;
      if (!source->Read(&stream->pending_name, &stream->pending_type, &stream->pending_ino)) {
        if (errno != 0 && used == 0) {
          stream->next = -1;  // the next call starts over
          return -1;
        }
        errno = saved;
        break;
      }
      errno = saved;
      stream->pending = true;
    }
    const size_t name_length = stream->pending_name.size();
    const size_t record = (kNameOffset + name_length + 1 + 7) & ~size_t{7};
    if (used + record > size) {
      // Kept: the next call starts with it.
      if (used == 0) {
        stream->next = index;
        return errno = EINVAL, -1;  // the buffer cannot hold one entry
      }
      break;
    }
    stream->pending = false;
    ++index;
    unsigned char* r = out + used;
    memset(r, 0, record);
    // d_ino: the file ID stat reports as st_ino; where there is none (a
    // /proc entry, a volume without IDs), the entry's position, as
    // readdir's (some code skips 0).
    const unsigned long long ino =
        stream->pending_ino != 0 ? stream->pending_ino : static_cast<unsigned long long>(index);
    const long long next = index;
    const unsigned short reclen = static_cast<unsigned short>(record);
    memcpy(r, &ino, 8);
    memcpy(r + 8, &next, 8);
    memcpy(r + 16, &reclen, 2);
    r[18] = stream->pending_type;
    memcpy(r + kNameOffset, stream->pending_name.data(), name_length);
    used += record;
  }
  // The stream's lock keeps this process's calls apart, but not an lseek,
  // nor a process sharing the description after fork: one that moved the
  // position meanwhile wins (as its lseek, ordered after this call, would on
  // Linux), and this stream starts over at its position next time.
  LARGE_INTEGER now = {};
  if (!SetFilePointerEx(h, zero, &now, FILE_CURRENT)) return gesher::FailWithLastError();
  if (now.QuadPart != read_at) {
    stream->next = -1;
    return static_cast<ssize_t>(used);
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
  const unsigned char type = EntryType(dir->data.dwFileAttributes, dir->data.dwReserved0);
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
// fork (gesher/fork.h): the attach lock, a leaf; the child closes the
// inherited stream handles and starts a new generation (AttachForkChild).
namespace {
const gesher::ForkLockOps kAttachOps = {&AttachTryLock, &AttachUnlock, &AttachForkChild};
const gesher::ForkLockRegistration g_attach_registration(
    gesher::kForkOrderLeaf, "g_attach_lock", []() -> void* { return &g_attach_lock; }, &kAttachOps);
}  // namespace
#endif
