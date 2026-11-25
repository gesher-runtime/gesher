// POSIX FIFOs (mkfifo, fifo(7)) shared by every process that opens them
// (ADR 0097).
//
// On disk a FIFO is a file carrying WSL's IO_REPARSE_TAG_LX_FIFO tag. Its
// pipe is a ring of kCapacity bytes in a section named after the FIFO's
// stable identity (volume serial and file ID), guarded by a named mutex. Any
// process of the same session (or AppContainer) that opens the path maps the
// same ring, so independent opens rendezvous, block and exchange bytes as
// Linux's single pipe object does.
//
// Each open creates an endpoint: the shared section of its open file
// description, created with a name and listed in the ring's slot table. The
// description section is inherited by exec and duplicated by dup like any
// other description, so an endpoint stays open exactly while some process
// holds a descriptor for it. Readers and writers are counted from the live
// slots: a slot whose named section no longer exists belongs to descriptors
// that are all closed (or whose processes have exited), and is dropped.
#include "fifo_internal.h"

#include "internal.h"
#include "ofd_internal.h"
#include "record_lock_descriptors.h"
#include "socket_user_copy.h"

// The fork set (gesher/fork.h); absent where a host test compiles this file alone.
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif

#include <bcrypt.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace gesher {

// The descriptor layer's public definition; kept with the FIFO's state.
struct FifoRing;
struct FifoEndpoint {
  std::wstring name;  // the description section's name
  std::shared_ptr<FifoRing> ring;
  uint64_t nonce[2] = {0, 0};
  void* view = nullptr;  // this endpoint's own view of the description section
  struct Trailer* trailer = nullptr;
  ~FifoEndpoint();
};

namespace {

constexpr uint32_t kCapacity = 65536;  // Linux's default pipe size
constexpr uint32_t kPipeBuf = 4096;    // PIPE_BUF: writes up to this are atomic
constexpr uint32_t kSlots = 64;        // open descriptions per FIFO
constexpr ULONGLONG kSweepMs = 100;    // how stale a dead endpoint may be
constexpr DWORD kWaitSliceMs = 10;
constexpr DWORD kRetireLockMs = 1000;  // a stopped lock holder never wedges a close
constexpr DWORD kSetupLockMs = 5000;   // ... nor an open (EAGAIN)
constexpr uint64_t kRingMagic = 0x3146464946534847ull;     // GSHFIFF1
constexpr uint64_t kTrailerMagic = 0x3145464946534847ull;  // GSHFIFE1
constexpr uint32_t kRead = 1, kWrite = 2;

struct Slot {
  uint64_t nonce[2];
  uint32_t role;  // kRead | kWrite; 0 when free
  uint32_t reserved[3];
};
static_assert(sizeof(Slot) == 32);

// Mutated only while the ring's named mutex is held.
struct RingHeader {
  uint64_t magic;
  uint32_t capacity, slot_count;
  uint64_t head, tail;  // bytes read and written since the pipe was created
  uint32_t reader_opens, writer_opens;  // Linux's r_counter and w_counter
  uint32_t readers, writers;  // live endpoints with that access
  uint64_t swept;  // GetTickCount64 of the last liveness sweep
  uint64_t reserved[2];
  Slot slots[kSlots];
};
constexpr size_t kRingSize = sizeof(RingHeader) + kCapacity;

}  // namespace

// The description section: the shared status every description has, then
// what identifies its FIFO. Written once before the endpoint is published.
struct Trailer {
  uint64_t magic;
  uint64_t key[3];  // volume serial, 128-bit file ID
  uint64_t nonce[2];
  uint32_t role;
  uint32_t writer_epoch;  // w_counter when opened: POLLHUP needs a later writer
  uint64_t device, inode;
  uint32_t mode, reserved;
  int64_t access_time, write_time, change_time;
  char path[1024];  // the FIFO's path when opened (UTF-8, Linux form; empty if longer)
};

namespace {

constexpr size_t kTrailerOffset = (sizeof(OpenFileDescription::Shared) + 63) & ~size_t{63};
constexpr size_t kDescriptionSize = kTrailerOffset + sizeof(Trailer);

}  // namespace

struct FifoRing {
  std::wstring base;
  HANDLE section = nullptr, lock = nullptr, wake = nullptr;
  RingHeader* header = nullptr;
  unsigned char* data = nullptr;
  ~FifoRing() {
    if (header) UnmapViewOfFile(header);
    for (HANDLE handle : {section, lock, wake}) if (handle) CloseHandle(handle);
  }
};

namespace {

void Hex(std::wstring* out, const uint64_t* values, size_t count) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  for (size_t i = 0; i < count; ++i)
    for (int shift = 60; shift >= 0; shift -= 4) *out += digits[(values[i] >> shift) & 15];
}

std::wstring BaseName(const uint64_t key[3]) {
  std::wstring name = L"Local\\gesher-fifo-";
  Hex(&name, key, 3);
  return name;
}

std::wstring EndpointName(const std::wstring& base, const uint64_t nonce[2]) {
  std::wstring name = base + L"-e";
  Hex(&name, nonce, 2);
  return name;
}

int Win32Errno() { return ErrnoFromWin32(GetLastError()); }

// The ring's named mutex. Waits that may block (a transfer or an open) also
// end on the caller's cancellation or interruption event (EINTR); others are
// bounded (EAGAIN), so a stopped holder in another process cannot wedge a
// poll timeout or a close here.
class RingLock {
 public:
  explicit RingLock(FifoRing& ring, DWORD timeout = INFINITE, HANDLE canceled = nullptr,
                    HANDLE interrupted = nullptr) : ring_(ring) {
    HANDLE handles[3] = {ring.lock, nullptr, nullptr};
    DWORD count = 1;
    if (canceled) handles[count++] = canceled;
    if (interrupted && interrupted != canceled) handles[count++] = interrupted;
    const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, timeout);
    // An abandoned mutex is held: every update publishes its counters after
    // the bytes, so a crashed holder leaves a consistent ring.
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED_0) held_ = true;
    else if (wait == WAIT_TIMEOUT) error_ = EAGAIN;
    else if (wait > WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + count) error_ = EINTR;
    else error_ = wait == WAIT_FAILED ? Win32Errno() : EIO;
  }
  ~RingLock() { if (held_) ReleaseMutex(ring_.lock); }
  RingLock(const RingLock&) = delete;
  RingLock& operator=(const RingLock&) = delete;
  int Error() const { return error_; }
 private:
  FifoRing& ring_;
  bool held_ = false;
  int error_ = 0;
};

std::mutex& CacheMutex() {
  static std::mutex* mutex = new std::mutex;  // process lifetime
  return *mutex;
}
std::map<std::wstring, std::weak_ptr<FifoRing>>& Cache() {
  static auto* cache = new std::map<std::wstring, std::weak_ptr<FifoRing>>;
  return *cache;
}

// The process's mapping of one FIFO's ring, shared by its descriptions.
std::shared_ptr<FifoRing> AcquireRing(const uint64_t key[3]) {
  const std::wstring base = BaseName(key);
  std::lock_guard<std::mutex> guard(CacheMutex());
  auto& cache = Cache();
  for (auto it = cache.begin(); it != cache.end();) it = it->second.expired() ? cache.erase(it) : std::next(it);
  auto found = cache.find(base);
  if (found != cache.end()) {
    if (auto ring = found->second.lock()) return ring;
  }
  auto ring = std::make_shared<FifoRing>();
  ring->base = base;
  ring->lock = CreateMutexW(nullptr, FALSE, (base + L"-lock").c_str());
  ring->wake = CreateEventW(nullptr, TRUE, FALSE, (base + L"-wake").c_str());
  ring->section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(kRingSize), base.c_str());
  if (!ring->lock || !ring->wake || !ring->section) { errno = Win32Errno(); return {}; }
  ring->header = static_cast<RingHeader*>(MapViewOfFile(ring->section, FILE_MAP_READ | FILE_MAP_WRITE,
                                                        0, 0, kRingSize));
  if (!ring->header) { errno = Win32Errno(); return {}; }
  ring->data = reinterpret_cast<unsigned char*>(ring->header + 1);
  {
    RingLock lock(*ring, kSetupLockMs);
    if (lock.Error()) { errno = lock.Error(); return {}; }
    RingHeader& header = *ring->header;
    if (header.magic == 0) {
      header.capacity = kCapacity;
      header.slot_count = kSlots;
      MemoryBarrier();
      header.magic = kRingMagic;
    } else if (header.magic != kRingMagic || header.capacity != kCapacity || header.slot_count != kSlots) {
      errno = EPROTO;
      return {};
    }
  }
  cache[base] = ring;
  return ring;
}

void Recount(RingHeader& header) {
  header.readers = header.writers = 0;
  for (const Slot& slot : header.slots) {
    header.readers += (slot.role & kRead) != 0;
    header.writers += (slot.role & kWrite) != 0;
  }
}

// Drops slots whose description no longer exists anywhere. The caller holds
// the ring lock. A section that cannot be probed for another reason (an
// access failure) stays counted: an endpoint is never invented as closed.
void Sweep(FifoRing& ring, bool force) {
  RingHeader& header = *ring.header;
  const ULONGLONG now = GetTickCount64();
  if (!force && now - header.swept < kSweepMs) return;
  header.swept = now;
  bool changed = false;
  for (Slot& slot : header.slots) {
    if (!slot.role) continue;
    HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, EndpointName(ring.base, slot.nonce).c_str());
    if (probe) { CloseHandle(probe); continue; }
    if (GetLastError() != ERROR_FILE_NOT_FOUND) continue;
    std::memset(&slot, 0, sizeof(slot));
    changed = true;
  }
  if (changed) {
    Recount(header);
    PulseEvent(ring.wake);
  }
}

void RemoveSlot(FifoRing& ring, const uint64_t nonce[2]) {
  RingHeader& header = *ring.header;
  for (Slot& slot : header.slots) {
    if (slot.role && slot.nonce[0] == nonce[0] && slot.nonce[1] == nonce[1]) {
      std::memset(&slot, 0, sizeof(slot));
      Recount(header);
      PulseEvent(ring.wake);
      return;
    }
  }
}

bool Signaled(HANDLE event) { return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }

// One bounded wait for the ring to change. Peers in other processes wake it
// through the named event; a missed pulse costs at most one slice.
void WaitSlice(FifoRing& ring, HANDLE canceled, HANDLE interrupted) {
  HANDLE handles[3];
  DWORD count = 0;
  handles[count++] = ring.wake;
  if (canceled) handles[count++] = canceled;
  if (interrupted && interrupted != canceled) handles[count++] = interrupted;
  WaitForMultipleObjects(count, handles, FALSE, kWaitSliceMs);
}

// The caller holds description.mutex.
FifoEndpoint* EndpointLocked(OpenFileDescription& description) {
  if (description.fifo) return description.fifo.get();
  if (!description.mapping) { errno = EBADF; return nullptr; }
  auto endpoint = std::make_shared<FifoEndpoint>();
  endpoint->view = MapViewOfFile(description.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kDescriptionSize);
  if (!endpoint->view) { errno = Win32Errno(); return nullptr; }
  auto* trailer = reinterpret_cast<Trailer*>(static_cast<unsigned char*>(endpoint->view) + kTrailerOffset);
  if (trailer->magic != kTrailerMagic) { errno = EBADF; return nullptr; }
  endpoint->trailer = trailer;
  endpoint->nonce[0] = trailer->nonce[0];
  endpoint->nonce[1] = trailer->nonce[1];
  endpoint->ring = AcquireRing(trailer->key);
  if (!endpoint->ring) return nullptr;
  endpoint->name = EndpointName(endpoint->ring->base, endpoint->nonce);
  {
    // A launcher passes the ring with an inherited description (exec_spawn
    // and FifoRingSection), so the ring outlives its exporter. Should it
    // still have been recreated, the endpoint registers again so that its
    // peers count it.
    RingLock lock(*endpoint->ring, kSetupLockMs);
    if (lock.Error()) { errno = lock.Error(); return nullptr; }
    RingHeader& header = *endpoint->ring->header;
    bool listed = false;
    Slot* free_slot = nullptr;
    for (Slot& slot : header.slots) {
      if (slot.role && slot.nonce[0] == endpoint->nonce[0] && slot.nonce[1] == endpoint->nonce[1]) listed = true;
      if (!slot.role && !free_slot) free_slot = &slot;
    }
    if (!listed) {
      if (!free_slot) { errno = ENFILE; return nullptr; }
      free_slot->nonce[0] = endpoint->nonce[0];
      free_slot->nonce[1] = endpoint->nonce[1];
      free_slot->role = trailer->role;
      Recount(header);
      PulseEvent(endpoint->ring->wake);
    }
  }
  description.fifo = std::move(endpoint);
  return description.fifo.get();
}

FifoEndpoint* Endpoint(const DescriptorSnapshotPtr& snapshot) {
  if (!snapshot || !snapshot->description || snapshot->kind != DescriptorKind::kFifo) {
    errno = EBADF;
    return nullptr;
  }
  std::lock_guard<std::mutex> guard(snapshot->description->mutex);
  return EndpointLocked(*snapshot->description);
}

int FileIdentity(HANDLE file, uint64_t key[3], BY_HANDLE_FILE_INFORMATION* info) {
  if (!GetFileInformationByHandle(file, info)) return Win32Errno();
  FILE_ID_INFO id{};
  if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
    key[0] = id.VolumeSerialNumber;
    std::memcpy(&key[1], id.FileId.Identifier, sizeof(id.FileId.Identifier));
  } else {
    // ReFS-less older volumes: the 64-bit index is unique per volume.
    key[0] = info->dwVolumeSerialNumber;
    key[1] = (static_cast<uint64_t>(info->nFileIndexHigh) << 32) | info->nFileIndexLow;
    key[2] = UINT64_MAX;
  }
  return 0;
}

int64_t Time(const FILETIME& value) {
  return static_cast<int64_t>((static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime);
}

// Waits for the other end as fifo_open's wait_for_partner does: until the
// partner count's open counter moves, so a peer that opens and closes again
// before this thread looks still completes the rendezvous.
int WaitForPartner(FifoRing& ring, bool reader, uint32_t start) {
  int previous = PTHREAD_CANCEL_DISABLE;
  int error = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
  if (error) return gesher_errno_from_crt(error);
  HANDLE canceled = previous == PTHREAD_CANCEL_ENABLE ? static_cast<HANDLE>(pthread_getevent()) : nullptr;
  if (previous == PTHREAD_CANCEL_ENABLE && !canceled) error = ENOMEM;
  while (!error) {
    {
      RingLock lock(ring, INFINITE, canceled);
      if (lock.Error()) { error = lock.Error(); break; }
      const RingHeader& header = *ring.header;
      if ((reader ? header.writer_opens : header.reader_opens) != start) break;
    }
    if (Signaled(canceled)) { error = EINTR; break; }
    WaitSlice(ring, canceled, nullptr);
  }
  pthread_setcancelstate(previous, nullptr);
  return error;
}

}  // namespace

FifoEndpoint::~FifoEndpoint() {
  if (view) UnmapViewOfFile(view);
  if (!ring || name.empty()) return;
  const int saved = errno;
  // The description's own section handle and view are already released (the
  // description destroys its members after closing them). If no process
  // holds the description any more, its endpoint closed: count it out now
  // rather than at the next sweep, so readers see end of file at once.
  HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (probe) {
    CloseHandle(probe);
  } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
    RingLock lock(*ring, kRetireLockMs);
    if (!lock.Error()) RemoveSlot(*ring, nonce);
  }
  errno = saved;
}

bool IsFifoHandle(HANDLE file) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  return GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
         (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && tag.ReparseTag == kReparseTagFifo;
}

HANDLE OpenFifoFollowing(const std::wstring& start, DWORD access, DWORD share, DWORD flags_and_attributes,
                         SECURITY_ATTRIBUTES* security) {
  // Windows follows every symbolic link of a path but its last component's
  // target, the FIFO, it refuses: walk the final links here.
  std::wstring path = start;
  for (int depth = 0; depth < 40; ++depth) {
    HANDLE file = CreateFileW(path.c_str(), access | FILE_READ_ATTRIBUTES, share, security, OPEN_EXISTING,
                              flags_and_attributes | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return file;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      CloseHandle(file);
      SetLastError(ERROR_CANT_ACCESS_FILE);
      return INVALID_HANDLE_VALUE;
    }
    if (tag.ReparseTag == kReparseTagFifo) return file;
    alignas(8) unsigned char buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD bytes = 0;
    const bool read = tag.ReparseTag == IO_REPARSE_TAG_SYMLINK &&
        DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer, sizeof(buffer), &bytes, nullptr);
    CloseHandle(file);
    struct Symlink {
      DWORD tag;
      WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
      ULONG flags;
    };
    if (!read || bytes < sizeof(Symlink)) {
      SetLastError(ERROR_CANT_ACCESS_FILE);
      return INVALID_HANDLE_VALUE;
    }
    Symlink link;
    std::memcpy(&link, buffer, sizeof(link));
    if (sizeof(Symlink) + link.substitute_offset + link.substitute_length > bytes) {
      SetLastError(ERROR_CANT_ACCESS_FILE);
      return INVALID_HANDLE_VALUE;
    }
    std::wstring target(reinterpret_cast<const wchar_t*>(buffer + sizeof(Symlink) + link.substitute_offset),
                        link.substitute_length / sizeof(wchar_t));
    if ((link.flags & 1) != 0) {  // SYMLINK_FLAG_RELATIVE
      const size_t slash = path.find_last_of(L"\\/");
      path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + target;
    } else if (target.rfind(L"\\??\\", 0) == 0) {
      path = L"\\\\?\\" + target.substr(4);
    } else {
      path = target;
    }
    // Resolve "." and ".." lexically, as Windows evaluates a symbolic link's
    // target; a \\?\ path would otherwise be taken literally.
    const bool literal = path.rfind(L"\\\\?\\", 0) == 0 && path.rfind(L"\\\\?\\UNC\\", 0) != 0;
    const std::wstring plain = literal ? path.substr(4) : path;
    DWORD length = GetFullPathNameW(plain.c_str(), 0, nullptr, nullptr);
    if (length != 0) {
      std::wstring full(length, L'\0');
      length = GetFullPathNameW(plain.c_str(), length, full.data(), nullptr);
      if (length != 0 && length < full.size()) {
        full.resize(length);
        path = literal ? L"\\\\?\\" + full : full;
      }
    }
  }
  SetLastError(ERROR_CANT_RESOLVE_FILENAME);
  return INVALID_HANDLE_VALUE;
}

HANDLE CreateFileFollowingFifo(const wchar_t* path, DWORD access, DWORD share, DWORD flags_and_attributes) {
  HANDLE file = CreateFileW(path, access, share, nullptr, OPEN_EXISTING, flags_and_attributes, nullptr);
  if (file != INVALID_HANDLE_VALUE || (flags_and_attributes & FILE_FLAG_OPEN_REPARSE_POINT) != 0 ||
      GetLastError() != ERROR_CANT_ACCESS_FILE) return file;
  return OpenFifoFollowing(path, access, share, flags_and_attributes, nullptr);
}

HANDLE OpenFifoFile(const DescriptorSnapshotPtr& snapshot, DWORD access) {
  FifoEndpoint* endpoint = Endpoint(snapshot);
  if (!endpoint) return INVALID_HANDLE_VALUE;
  std::wstring windows;
  if (!endpoint->trailer->path[0] || MapPath(endpoint->trailer->path, &windows) != PathKind::kFile) {
    errno = EOPNOTSUPP;
    return INVALID_HANDLE_VALUE;
  }
  HANDLE file = CreateFileW(windows.c_str(), access | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  uint64_t key[3] = {0, 0, 0};
  BY_HANDLE_FILE_INFORMATION info{};
  // Renamed away or replaced since it was opened: the FIFO has no name here.
  if (file == INVALID_HANDLE_VALUE || FileIdentity(file, key, &info) != 0 ||
      std::memcmp(key, endpoint->trailer->key, sizeof(key)) != 0) {
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    errno = EOPNOTSUPP;
    return INVALID_HANDLE_VALUE;
  }
  return file;
}

int OpenFifoHandle(HANDLE probe, int flags) {
  struct Owned {
    HANDLE probe = nullptr, section = nullptr, placeholder = nullptr;
    void* view = nullptr;
    void Release() {
      if (view) UnmapViewOfFile(view);
      for (HANDLE handle : {probe, section, placeholder}) if (handle) CloseHandle(handle);
      view = probe = section = placeholder = nullptr;
    }
    ~Owned() { Release(); }
  } owned;
  owned.probe = probe;
  if ((flags & O_DIRECTORY) != 0) return errno = ENOTDIR, -1;
  const uint32_t role = (flags & O_ACCMODE) == O_RDONLY ? kRead : (flags & O_ACCMODE) == O_WRONLY ? kWrite
                                                                                                   : kRead | kWrite;
  const bool nonblocking = (flags & O_NONBLOCK) != 0;
  uint64_t key[3] = {0, 0, 0};
  BY_HANDLE_FILE_INFORMATION info{};
  if (int error = FileIdentity(probe, key, &info)) return errno = error, -1;
  std::shared_ptr<FifoRing> ring = AcquireRing(key);
  if (!ring) return -1;

  // The description section, named so that every copy of it (dup, exec)
  // keeps the endpoint alive and the ring can tell when all are gone.
  uint64_t nonce[2] = {0, 0};
  std::wstring name;
  for (int attempt = 0; attempt < 4 && !owned.section; ++attempt) {
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce), sizeof(nonce),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return errno = EIO, -1;
    name = EndpointName(ring->base, nonce);
    owned.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(kDescriptionSize), name.c_str());
    if (!owned.section) return errno = Win32Errno(), -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(owned.section); owned.section = nullptr; }
  }
  if (!owned.section) return errno = EEXIST, -1;
  owned.view = MapViewOfFile(owned.section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kDescriptionSize);
  if (!owned.view) return errno = Win32Errno(), -1;
  auto* shared = static_cast<OpenFileDescription::Shared*>(owned.view);
  std::memset(owned.view, 0, kDescriptionSize);
  shared->magic = ofd_transfer::kMagic;
  shared->version = ofd_transfer::kVersion;
  const int status = (flags & kOpenDescriptionFlagsMask & ~(O_PATH | O_DIRECTORY | O_NOFOLLOW)) | O_LARGEFILE;
  InterlockedExchange(&shared->flags, static_cast<LONG>(status));
  auto* trailer = reinterpret_cast<Trailer*>(static_cast<unsigned char*>(owned.view) + kTrailerOffset);
  std::memcpy(trailer->key, key, sizeof(key));
  std::memcpy(trailer->nonce, nonce, sizeof(nonce));
  trailer->role = role;
  trailer->device = info.dwVolumeSerialNumber;
  trailer->inode = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  trailer->mode = (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0 ? 0444 : 0666;
  trailer->access_time = Time(info.ftLastAccessTime);
  trailer->write_time = Time(info.ftLastWriteTime);
  trailer->change_time = Time(info.ftLastWriteTime);
  {
    wchar_t final_path[1024];
    const DWORD length = GetFinalPathNameByHandleW(owned.probe, final_path, 1024,
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length != 0 && length < 1024) {
      const std::string linux_path = LinuxPath(final_path);
      if (linux_path.size() < sizeof(trailer->path))
        std::memcpy(trailer->path, linux_path.c_str(), linux_path.size() + 1);
    }
  }
  CloseHandle(owned.probe);
  owned.probe = nullptr;

  // Register the endpoint, as fifo_open counts it before any wait.
  bool wait = false;
  uint32_t start = 0;
  {
    RingLock lock(*ring, kSetupLockMs);
    if (lock.Error()) return errno = lock.Error(), -1;
    Sweep(*ring, true);
    RingHeader& header = *ring->header;
    // The last close of a Linux FIFO frees its pipe: nothing written before
    // survives into the next open.
    if (header.readers == 0 && header.writers == 0) header.head = header.tail = 0;
    if (role == kWrite && nonblocking && header.readers == 0) return errno = ENXIO, -1;
    Slot* free_slot = nullptr;
    for (Slot& slot : header.slots) if (!slot.role) { free_slot = &slot; break; }
    if (!free_slot) return errno = ENFILE, -1;
    free_slot->nonce[0] = nonce[0];
    free_slot->nonce[1] = nonce[1];
    free_slot->role = role;
    if (role & kRead) ++header.reader_opens;
    if (role & kWrite) ++header.writer_opens;
    Recount(header);
    trailer->writer_epoch = header.writer_opens;
    MemoryBarrier();
    trailer->magic = kTrailerMagic;
    if (!nonblocking && role == kRead && header.writers == 0) wait = true, start = header.writer_opens;
    if (!nonblocking && role == kWrite && header.readers == 0) wait = true, start = header.reader_opens;
    PulseEvent(ring->wake);
  }
  auto unregister = [&] {
    RingLock lock(*ring, kRetireLockMs);
    if (!lock.Error()) RemoveSlot(*ring, nonce);
  };
  if (wait) {
    if (int error = WaitForPartner(*ring, role == kRead, start)) {
      unregister();
      // winpthreads cancels by unwinding without destructors: act on a
      // pending cancellation only once this open holds nothing.
      owned.Release();
      ring.reset();
      name = std::wstring();
      errno = error;
      pthread_testcancel();
      return -1;
    }
  }

  // The descriptor: a pipe-type placeholder without data access, so that the
  // descriptor layer treats it as a stream (ESPIPE, EINVAL for fsync and
  // ftruncate) and dispatches its I/O here by kind.
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, nullptr, 1)) {
    const int error = Win32Errno();
    unregister();
    return errno = error, -1;
  }
  CloseHandle(write_end);
  const bool inherit = (flags & O_CLOEXEC) == 0;
  if (!DuplicateHandle(GetCurrentProcess(), read_end, GetCurrentProcess(), &owned.placeholder,
                       FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE, inherit, 0)) {
    const int error = Win32Errno();
    CloseHandle(read_end);
    unregister();
    return errno = error, -1;
  }
  CloseHandle(read_end);
  record_locks::DescriptorGuard descriptors;
  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(owned.placeholder),
                                 _O_BINARY | (role == kRead ? _O_RDONLY : 0) | (inherit ? 0 : _O_NOINHERIT));
  if (fd < 0) {
    const int error = gesher_errno_from_crt(errno);
    unregister();
    return errno = error, -1;
  }
  owned.placeholder = nullptr;
  std::shared_ptr<OpenFileDescription> description;
  int error = RegisterOfd(fd, status) < 0 ? errno : 0;
  if (!error && !LookupOfd(fd, &description)) error = EBADF;
  if (!error) {
    std::lock_guard<std::mutex> guard(description->mutex);
    description->mapping = owned.section;
    description->shared.store(shared, std::memory_order_release);
    description->SetFlags(status);
    description->kind = DescriptorKind::kFifo;
    owned.section = nullptr;
    owned.view = nullptr;
    // Bind the endpoint now, so that closing the descriptor counts it out
    // at once even if it never transferred a byte.
    if (!EndpointLocked(*description)) error = errno;
  }
  if (error) {
    _close(fd);
    ForgetOfd(fd);
    unregister();
    return errno = error, -1;
  }
  return fd;
}

int TransferFifo(const DescriptorSnapshotPtr& snapshot, void* bytes, unsigned count, bool writing,
                 HANDLE canceled, HANDLE interrupted) {
  FifoEndpoint* endpoint = Endpoint(snapshot);
  if (!endpoint) return -1;
  FifoRing& ring = *endpoint->ring;
  auto* buffer = static_cast<unsigned char*>(bytes);
  unsigned done = 0;
  for (;;) {
    if (Signaled(canceled) || Signaled(interrupted)) return done ? static_cast<int>(done) : (errno = EINTR, -1);
    const bool nonblocking = (snapshot->description->Flags() & O_NONBLOCK) != 0;
    {
      RingLock lock(ring, nonblocking ? kRetireLockMs : INFINITE, canceled, interrupted);
      if (lock.Error() == EINTR) return done ? static_cast<int>(done) : (errno = EINTR, -1);
      if (lock.Error()) return done ? static_cast<int>(done) : (errno = lock.Error(), -1);
      Sweep(ring, false);
      RingHeader& header = *ring.header;
      const uint64_t used = header.tail - header.head;
      if (!writing) {
        if (used) {
          const unsigned n = static_cast<unsigned>(std::min<uint64_t>(count, used));
          const uint32_t at = static_cast<uint32_t>(header.head % kCapacity);
          const unsigned first = std::min(n, kCapacity - at);
          if (!ws::usercopy::To(buffer, ring.data + at, first) ||
              !ws::usercopy::To(buffer + first, ring.data, n - first)) return errno = EFAULT, -1;
          header.head += n;
          PulseEvent(ring.wake);
          return static_cast<int>(n);
        }
        if (header.writers == 0) return 0;
        if (nonblocking) {
          // EAGAIN only for a writer that really exists: a writer whose
          // last process has just exited means end of file instead.
          Sweep(ring, true);
          return header.writers == 0 ? 0 : (errno = EAGAIN, -1);
        }
      } else {
        if (header.readers == 0) return done ? static_cast<int>(done) : (errno = EPIPE, -1);
        const uint64_t room = kCapacity - used;
        // Up to PIPE_BUF bytes go in as one record or not at all; a larger
        // write may be split, and interleave with other writers, as on Linux.
        unsigned n = 0;
        if (count <= kPipeBuf) n = room >= count ? count : 0;
        else n = static_cast<unsigned>(std::min<uint64_t>(room, count - done));
        if (n) {
          const uint32_t at = static_cast<uint32_t>(header.tail % kCapacity);
          const unsigned first = std::min(n, kCapacity - at);
          if (!ws::usercopy::From(ring.data + at, buffer + done, first) ||
              !ws::usercopy::From(ring.data, buffer + done + first, n - first))
            return done ? static_cast<int>(done) : (errno = EFAULT, -1);
          header.tail += n;
          done += n;
          PulseEvent(ring.wake);
          if (done == count) return static_cast<int>(done);
        }
        if (nonblocking) return done ? static_cast<int>(done) : (errno = EAGAIN, -1);
      }
    }
    WaitSlice(ring, canceled, interrupted);
  }
}

int PollFifo(const DescriptorSnapshotPtr& snapshot, short events, short* ready) {
  *ready = 0;
  FifoEndpoint* endpoint = Endpoint(snapshot);
  if (!endpoint) return -1;
  const int access = snapshot->description->Flags() & O_ACCMODE;
  FifoRing& ring = *endpoint->ring;
  // One poll slice at most: a peer stopped while holding the lock reads as
  // "nothing ready yet", and the poll's own timeout still applies.
  RingLock lock(ring, kWaitSliceMs);
  if (lock.Error() == EAGAIN) return 0;
  if (lock.Error()) return errno = lock.Error(), -1;
  Sweep(ring, false);
  const RingHeader& header = *ring.header;
  const uint64_t used = header.tail - header.head;
  short result = 0;
  if (access != O_WRONLY) {
    if (used) result |= events & (POLLIN | POLLRDNORM);
    // As pipe_poll: a reader hangs up only after a writer came and went.
    if (header.writers == 0 && endpoint->trailer->writer_epoch != header.writer_opens) result |= POLLHUP;
  }
  if (access != O_RDONLY) {
    if (header.readers == 0) result |= POLLERR;
    if (kCapacity - used >= kPipeBuf) result |= events & (POLLOUT | POLLWRNORM);
  }
  *ready = result;
  return 0;
}

int FifoBytesAvailable(const DescriptorSnapshotPtr& snapshot, int* bytes) {
  if (!bytes) return errno = EFAULT, -1;
  FifoEndpoint* endpoint = Endpoint(snapshot);
  if (!endpoint) return -1;
  RingLock lock(*endpoint->ring, kRetireLockMs);
  if (lock.Error()) return errno = lock.Error(), -1;
  const RingHeader& header = *endpoint->ring->header;
  *bytes = static_cast<int>(header.tail - header.head);
  return 0;
}

HANDLE FifoRingSection(const DescriptorSnapshotPtr& snapshot) {
  FifoEndpoint* endpoint = Endpoint(snapshot);
  return endpoint ? endpoint->ring->section : nullptr;
}

int FifoStatus(const DescriptorSnapshotPtr& snapshot, FifoFileStatus* status) {
  FifoEndpoint* endpoint = Endpoint(snapshot);
  if (!endpoint) return -1;
  const Trailer& trailer = *endpoint->trailer;
  status->device = trailer.device;
  status->inode = trailer.inode;
  status->mode = trailer.mode;
  status->access_time = trailer.access_time;
  status->write_time = trailer.write_time;
  status->change_time = trailer.change_time;
  return 0;
}

namespace {

// An inherited FIFO description binds its ring while the process starts.
// The launcher's inherited ring reference keeps the ring until then and stays
// open for this process's lifetime (its value is not passed in the manifest).
void BindInheritedFifos() {}
struct InheritedFifoBinding {
  InheritedFifoBinding() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(&BindInheritedFifos)), &self) ||
        self != GetModuleHandleW(nullptr)) return;
    const int saved = errno;
    std::vector<std::pair<int, DescriptorSnapshotPtr>> descriptors;
    if (CaptureDescriptors(&descriptors, true) == 0) {
      for (const auto& entry : descriptors)
        if (entry.second->kind == DescriptorKind::kFifo) Endpoint(entry.second);
    }
    errno = saved;
  }
};
InheritedFifoBinding inherited_fifo_binding;

}  // namespace

}  // namespace gesher

#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h): the process's table of FIFO rings. An open holds it
// while it maps a ring (and waits for the ring's lock); a child cloned then
// would find it held by a thread it does not have, and its next FIFO open
// would wait for ever.
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::CacheMutex());
#endif

// mkfifo(3) and mknod(S_IFIFO): an empty file with WSL's FIFO tag. A volume
// without reparse points (FAT) cannot hold one; Linux's vfat fails with EPERM.
extern "C" int mkfifo(const char* path, mode_t mode) {
  if (path == nullptr) return errno = EFAULT, -1;
  std::wstring windows;
  if (!gesher::DiskPath(path, &windows, EEXIST)) return -1;
  // The name appears already tagged, or not at all: the FIFO is made under a
  // private name in the same directory and renamed without replacing, so a
  // concurrent creator that gets EEXIST never sees a regular file there.
  if (GetFileAttributesW(windows.c_str()) != INVALID_FILE_ATTRIBUTES) return errno = EEXIST, -1;
  uint64_t nonce[2] = {0, 0};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(nonce), sizeof(nonce),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return errno = EIO, -1;
  const size_t slash = windows.find_last_of(L"\\/");
  std::wstring temporary = (slash == std::wstring::npos ? std::wstring() : windows.substr(0, slash + 1)) +
                           L".gesher-fifo-";
  gesher::Hex(&temporary, nonce, 2);
  // As open(O_CREAT) does here: no owner write permission makes it read-only.
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, 0, nullptr,
                            CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS |
                                ((mode & 0200) == 0 ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL),
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) return gesher::FailWithLastError();
  auto discard = [&](int error) {
    FILE_DISPOSITION_INFO_EX remove{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                    FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (!SetFileInformationByHandle(file, FileDispositionInfoEx, &remove, sizeof(remove))) {
      FILE_DISPOSITION_INFO fallback{TRUE};
      SetFileInformationByHandle(file, FileDispositionInfo, &fallback, sizeof(fallback));
    }
    CloseHandle(file);
    return errno = error, -1;
  };
  struct { DWORD tag; WORD length, reserved; } reparse{gesher::kReparseTagFifo, 0, 0};
  DWORD returned = 0;
  if (!DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, &reparse, sizeof(reparse), nullptr, 0, &returned, nullptr)) {
    const DWORD error = GetLastError();
    return discard(error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
                   error == ERROR_INVALID_PARAMETER ? EPERM : gesher::ErrnoFromWin32(error));
  }
  const size_t bytes = sizeof(FILE_RENAME_INFO) + windows.size() * sizeof(wchar_t);
  std::unique_ptr<unsigned char[]> storage(new (std::nothrow) unsigned char[bytes]());
  if (!storage) return discard(ENOMEM);
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(windows.size() * sizeof(wchar_t));
  std::memcpy(rename->FileName, windows.c_str(), windows.size() * sizeof(wchar_t));
  if (!SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(bytes))) {
    const DWORD error = GetLastError();
    return discard(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? EEXIST
                                                                               : gesher::ErrnoFromWin32(error));
  }
  CloseHandle(file);
  return 0;
}

// For bionic's truncate(2), which must not open a FIFO (EINVAL, as Linux).
extern "C" int gesher_is_fifo_path(const char* path) {
  std::wstring windows;
  if (path == nullptr || gesher::MapPath(path, &windows) != gesher::PathKind::kFile) return 0;
  HANDLE file = gesher::CreateFileFollowingFifo(windows.c_str(), FILE_READ_ATTRIBUTES,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                FILE_FLAG_BACKUP_SEMANTICS);
  if (file == INVALID_HANDLE_VALUE) return 0;
  const bool fifo = gesher::IsFifoHandle(file);
  CloseHandle(file);
  return fifo ? 1 : 0;
}
