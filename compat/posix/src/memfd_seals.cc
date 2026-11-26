// gesher-base: memfd seals (fcntl F_ADD_SEALS / F_GET_SEALS).
//
// A memfd is a delete-on-close temporary file (system.cc). Linux keeps its
// seals on the file (the inode), so every descriptor of it, in any process,
// sees and obeys the same seals. Here they are an NTFS extended attribute of
// the file, GESHER.SEALS, which any handle of it reads and writes: a memfd
// passed to another process (Binder, exec) keeps its seals there too.
//
// Linux refuses writes the seals forbid in the kernel. Windows cannot, so
// gesher-base refuses them in its own write paths: write/pwrite (and the
// vector forms built on them), ftruncate, fallocate, and MAP_SHARED writable
// mmap/mprotect (mman.cc). Descriptor registration supports fcntl and aliases;
// I/O checks the pinned handle and caches only whether its open description is
// a memfd. It always rereads a memfd's seals, which another process may add.
// A bounded atomic bitmap classifies registered descriptors in the common
// range without a registry mutex; large descriptor numbers use the registry.
//
// F_SEAL_WRITE requires that no writable shared mapping of the file exists,
// in any process, and answers EBUSY while one does (as Linux does). A writable
// shared mapping holds a lease for as long as it lives: a shared LockFileEx
// lock on a byte far beyond the data, taken through a handle of its own (the
// descriptor may be closed once the mapping exists). Adding F_SEAL_WRITE needs
// the same byte exclusively: it fails at once if any lease exists, and a
// mapping made after it sees the seal. mman.cc releases the lease when the
// last writable page of the mapping is unmapped or the mapping is replaced; the
// OS releases it when the process dies. Every change of the stored seals reads,
// checks and writes them under one more such byte, so concurrent additions in
// different processes cannot overwrite each other.
// F_SEAL_FUTURE_WRITE, which Android's ashmem-over-memfd uses, has no such
// condition.
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>

#include "internal.h"
#include "record_lock_liveness.h"
// The fork set (gesher/fork.h); absent where a host test compiles this file alone.
#if __has_include(<gesher/fork.h>)
#include <gesher/fork.h>
#endif

namespace gesher {
namespace {

constexpr char kSealsName[] = "GESHER.SEALS";
constexpr size_t kSealsNameLength = sizeof(kSealsName) - 1;
constexpr uint32_t kLinuxSeals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE |
                                 F_SEAL_FUTURE_WRITE;

struct IoStatus {
  union {
    LONG status;
    void* pointer;
  };
  ULONG_PTR information;
};
using SetEaFn = LONG(NTAPI*)(HANDLE, IoStatus*, void*, ULONG);
using QueryEaFn = LONG(NTAPI*)(HANDLE, IoStatus*, void*, ULONG, BOOLEAN, void*, ULONG, ULONG*, BOOLEAN);

struct EaApi {
  SetEaFn set = nullptr;
  QueryEaFn query = nullptr;
};

const EaApi& Api() {
  static const EaApi api = [] {
    EaApi a;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
      a.set = reinterpret_cast<SetEaFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetEaFile")));
      a.query = reinterpret_cast<QueryEaFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryEaFile")));
    }
    return a;
  }();
  return api;
}

// FILE_FULL_EA_INFORMATION with one 4-byte value.
struct FullEa {
  ULONG next;
  UCHAR flags;
  UCHAR name_length;
  USHORT value_length;
  char name[kSealsNameLength + 1];
  uint32_t value;
};
// FILE_GET_EA_INFORMATION naming GESHER.SEALS.
struct GetEa {
  ULONG next;
  UCHAR name_length;
  char name[kSealsNameLength + 1];
};

// The file's stored value, or false if it has none (not a memfd).
bool ReadValue(HANDLE file, uint32_t* value) {
  const EaApi& api = Api();
  if (api.query == nullptr) return false;
  GetEa list = {};
  list.name_length = static_cast<UCHAR>(kSealsNameLength);
  memcpy(list.name, kSealsName, kSealsNameLength + 1);
  alignas(8) unsigned char out[64] = {};
  IoStatus io = {};
  if (api.query(file, &io, out, sizeof(out), TRUE, &list, sizeof(list), nullptr, TRUE) < 0) return false;
  FullEa ea;
  memcpy(&ea, out, sizeof(ea) < sizeof(out) ? sizeof(ea) : sizeof(out));
  // An absent attribute comes back with an empty value.
  if (ea.value_length != sizeof(uint32_t)) return false;
  memcpy(value, out + offsetof(FullEa, name) + ea.name_length + 1, sizeof(*value));
  return true;
}

bool WriteValue(HANDLE file, uint32_t value) {
  const EaApi& api = Api();
  if (api.set == nullptr) return errno = EOPNOTSUPP, false;
  alignas(8) unsigned char in[sizeof(FullEa) + 8] = {};
  FullEa ea = {};
  ea.name_length = static_cast<UCHAR>(kSealsNameLength);
  ea.value_length = sizeof(uint32_t);
  memcpy(ea.name, kSealsName, kSealsNameLength + 1);
  memcpy(in, &ea, offsetof(FullEa, value));
  memcpy(in + offsetof(FullEa, name) + kSealsNameLength + 1, &value, sizeof(value));
  IoStatus io = {};
  const LONG status = api.set(file, &io, in, sizeof(in));
  if (status < 0) return errno = EIO, false;
  return true;
}

// Two bytes beyond any file's data, next to flock's (system.cc), where LockFileEx
// locks block nothing but other locks: a shared lock per live writable shared
// mapping (exclusive while F_SEAL_WRITE is added), and an exclusive lock while
// the seals are changed. They are polled, never waited on: a blocking
// LockFileEx holds a synchronous handle against the unlock of another thread.
constexpr DWORD kLeaseByte = 0xfffffffdu;
constexpr DWORD kChangeByte = 0xfffffffcu;
constexpr DWORD kLockByteHigh = 0x7fffffffu;
// Holders keep a byte for a few extended-attribute calls only.
constexpr int kLockAttempts = 5000;

// Serializes this process's seal changes and lease requests, so that they never
// meet through one handle, which Windows lets neither share nor upgrade a lock.
std::mutex g_seal_mutex;

// ERROR_SUCCESS, or ERROR_LOCK_VIOLATION after the attempts, or another error.
DWORD LockByte(HANDLE file, DWORD byte, bool exclusive, bool wait) {
  for (int attempt = 0;; ++attempt) {
    OVERLAPPED at = {};
    at.Offset = byte;
    at.OffsetHigh = kLockByteHigh;
    if (LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1,
                   0, &at)) {
      return ERROR_SUCCESS;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_LOCK_VIOLATION || !wait || attempt >= kLockAttempts) return error;
    Sleep(1);
  }
}

void UnlockByte(HANDLE file, DWORD byte) {
  OVERLAPPED at = {};
  at.Offset = byte;
  at.OffsetHigh = kLockByteHigh;
  UnlockFileEx(file, 0, 1, 0, &at);
}

int LockError(DWORD error, int busy) {
  return error == ERROR_LOCK_VIOLATION ? busy : ErrnoFromWin32(error);
}

// One writable shared mapping's claim on its file (see the top of this file).
class WriteLease {
 public:
  WriteLease() = default;
  WriteLease(const WriteLease&) = delete;
  WriteLease& operator=(const WriteLease&) = delete;
  ~WriteLease() {
    if (own_ == nullptr) return;
    UnlockByte(own_, kLeaseByte);
    CloseHandle(own_);
  }
  // In a forked child (ADR 0092): the child inherited the mapping and this
  // handle, but the byte-range lock is the parent's. The child takes its own
  // shared lock, so F_SEAL_WRITE stays EBUSY while the child maps the file.
  void RenewInChild() {
    if (own_ != nullptr && LockByte(own_, kLeaseByte, false, true) != ERROR_SUCCESS) {
      CloseHandle(own_);  // no lease the child could release
      own_ = nullptr;
    }
  }
  // 0, or the errno that denies the mapping; nothing is held after an error.
  int Acquire(HANDLE file) {
    HANDLE own = nullptr;
    // The same file object under a handle that outlives the descriptor's.
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &own, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
      return ErrnoFromWin32(GetLastError());
    }
    const DWORD error = LockByte(own, kLeaseByte, false, true);
    if (error != ERROR_SUCCESS) {
      CloseHandle(own);
      return LockError(error, EAGAIN);
    }
    own_ = own;
    return 0;
  }

 private:
  HANDLE own_ = nullptr;
};

// Holds the seals' change byte; Locked() is false (errno set) if it could not.
class SealChange {
 public:
  explicit SealChange(HANDLE file) : file_(file) {
    const DWORD error = LockByte(file, kChangeByte, true, true);
    if (error != ERROR_SUCCESS) {
      errno = LockError(error, EAGAIN);
      file_ = nullptr;
    }
  }
  SealChange(const SealChange&) = delete;
  SealChange& operator=(const SealChange&) = delete;
  ~SealChange() {
    if (file_ != nullptr) UnlockByte(file_, kChangeByte);
  }
  bool Locked() const { return file_ != nullptr; }

 private:
  HANDLE file_;
};

std::mutex g_registry_mutex;
std::unordered_set<int> g_registry;
std::atomic<size_t> g_registry_count{0};
constexpr unsigned kMemfdFastDescriptors = 65536;
constexpr unsigned kMemfdBitsPerWord = 64;
std::array<std::atomic<uint64_t>, kMemfdFastDescriptors / kMemfdBitsPerWord> g_memfd_descriptors{};
static_assert(std::atomic<uint64_t>::is_always_lock_free);

}  // namespace

bool InitMemfdSeals(HANDLE file, bool allow_sealing) {
  // Without MFD_ALLOW_SEALING, Linux reports F_SEAL_SEAL: no seal can be added.
  return WriteValue(file, allow_sealing ? 0u : static_cast<uint32_t>(F_SEAL_SEAL));
}

void RegisterMemfd(int fd) {
  if (fd < 0) return;
  if (static_cast<unsigned>(fd) < kMemfdFastDescriptors) {
    g_memfd_descriptors[fd / kMemfdBitsPerWord].fetch_or(
        uint64_t{1} << (fd % kMemfdBitsPerWord), std::memory_order_release);
    return;
  }
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (g_registry.insert(fd).second) g_registry_count.fetch_add(1, std::memory_order_release);
}

void ForgetMemfd(int fd) {
  if (fd < 0) return;
  if (static_cast<unsigned>(fd) < kMemfdFastDescriptors) {
    auto& word = g_memfd_descriptors[fd / kMemfdBitsPerWord];
    const uint64_t bit = uint64_t{1} << (fd % kMemfdBitsPerWord);
    // Ordinary closes must not contend for a writable cache line when their
    // descriptor has no memfd metadata. Descriptor retirement/reuse already
    // serializes registrations for this fd; other bits can change concurrently.
    if ((word.load(std::memory_order_acquire) & bit) != 0) {
      word.fetch_and(~bit, std::memory_order_release);
    }
    return;
  }
  if (g_registry_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (g_registry.erase(fd) != 0) g_registry_count.fetch_sub(1, std::memory_order_release);
}

bool IsRegisteredMemfd(int fd) {
  if (fd < 0) return false;
  if (static_cast<unsigned>(fd) < kMemfdFastDescriptors) {
    return (g_memfd_descriptors[fd / kMemfdBitsPerWord].load(std::memory_order_acquire) &
            (uint64_t{1} << (fd % kMemfdBitsPerWord))) != 0;
  }
  if (g_registry_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  return g_registry.count(fd) != 0;
}

void RegisterMemfdIfSealed(int fd, HANDLE file) {
  uint32_t value = 0;
  if (file != INVALID_HANDLE_VALUE && file != nullptr && ReadValue(file, &value)) RegisterMemfd(fd);
}

void CopyMemfdRegistration(int from, int to) {
  if (IsRegisteredMemfd(from)) RegisterMemfd(to);
  else ForgetMemfd(to);
}

int GetMemfdSeals(int fd) {
  HANDLE file = HandleFromFd(fd);
  if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  uint32_t value = 0;
  if (!ReadValue(file, &value)) return errno = EINVAL, -1;  // not a memfd: Linux's EINVAL
  RegisterMemfd(fd);
  return static_cast<int>(value & kLinuxSeals);
}

int AddMemfdSeals(int fd, unsigned seals) {
  HANDLE file = HandleFromFd(fd);
  if (file == INVALID_HANDLE_VALUE) return errno = EBADF, -1;
  if ((seals & ~kLinuxSeals) != 0) return errno = EINVAL, -1;
  uint32_t current = 0;
  if (!ReadValue(file, &current)) return errno = EINVAL, -1;
  RegisterMemfd(fd);
  // Linux needs a descriptor open for writing to add seals.
  uint32_t access = 0;
  if (record_locks::HandleAccess(file, &access) != 0 || (access & FILE_WRITE_DATA) == 0) {
    return errno = EPERM, -1;
  }
  if ((current & F_SEAL_SEAL) != 0) return errno = EPERM, -1;
  // Read, check and write under the change byte: another process may be adding
  // seals, and what it stored since the first read must not be overwritten.
  std::lock_guard<std::mutex> guard(g_seal_mutex);
  SealChange change(file);
  if (!change.Locked()) return -1;
  if (!ReadValue(file, &current)) return errno = EINVAL, -1;
  if ((current & F_SEAL_SEAL) != 0) return errno = EPERM, -1;
  if ((current & seals) == seals) return 0;
  if ((seals & F_SEAL_WRITE) != 0 && (current & F_SEAL_WRITE) == 0) {
    // No lease may exist while the seal is stored: a mapping made from now on
    // finds the seal, and one made before holds its lease and is refused here.
    const DWORD error = LockByte(file, kLeaseByte, true, false);
    if (error != ERROR_SUCCESS) return errno = LockError(error, EBUSY), -1;
    const bool stored = WriteValue(file, current | seals);
    UnlockByte(file, kLeaseByte);
    return stored ? 0 : -1;
  }
  return WriteValue(file, current | seals) ? 0 : -1;
}

bool FileHasMemfdSeals(HANDLE file) {
  uint32_t value = 0;
  return file != INVALID_HANDLE_VALUE && file != nullptr && ReadValue(file, &value);
}

int MemfdWriteHandleError(HANDLE file, bool at_position, uint64_t offset,
                          uint64_t length, bool append) {
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals)) return 0;
  if ((seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) != 0) return EPERM;
  if ((seals & F_SEAL_GROW) != 0 && length != 0) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) return ErrnoFromWin32(GetLastError());
    if (append) offset = static_cast<uint64_t>(size.QuadPart);
    else if (at_position) {
      LARGE_INTEGER zero{}, position{};
      if (!SetFilePointerEx(file, zero, &position, FILE_CURRENT)) return ErrnoFromWin32(GetLastError());
      offset = static_cast<uint64_t>(position.QuadPart);
    }
    if (offset > static_cast<uint64_t>(size.QuadPart) ||
        length > static_cast<uint64_t>(size.QuadPart) - offset) return EPERM;
  }
  return 0;
}

int MemfdWriteError(int fd, bool at_position, uint64_t offset, uint64_t length) {
  if (!IsRegisteredMemfd(fd)) return 0;
  return MemfdWriteHandleError(HandleFromFd(fd), at_position, offset, length, false);
}

int MemfdResizeHandleError(HANDLE file, uint64_t new_size) {
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals)) return 0;
  if ((seals & (F_SEAL_GROW | F_SEAL_SHRINK)) == 0) return 0;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) return ErrnoFromWin32(GetLastError());
  const uint64_t current = static_cast<uint64_t>(size.QuadPart);
  if (new_size > current && (seals & F_SEAL_GROW) != 0) return EPERM;
  if (new_size < current && (seals & F_SEAL_SHRINK) != 0) return EPERM;
  return 0;
}

int MemfdResizeError(int fd, uint64_t new_size) {
  if (!IsRegisteredMemfd(fd)) return 0;
  return MemfdResizeHandleError(HandleFromFd(fd), new_size);
}

int MemfdGrowHandleError(HANDLE file, uint64_t end) {
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals) || (seals & F_SEAL_GROW) == 0) return 0;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) return ErrnoFromWin32(GetLastError());
  return end > static_cast<uint64_t>(size.QuadPart) ? EPERM : 0;
}

int MemfdGrowError(int fd, uint64_t end) {
  if (!IsRegisteredMemfd(fd)) return 0;
  return MemfdGrowHandleError(HandleFromFd(fd), end);
}

int MemfdSharedMapHandleError(HANDLE file, bool writable, bool* deny_write,
                              std::shared_ptr<void>* write_lease) {
  *deny_write = false;
  if (write_lease != nullptr) write_lease->reset();
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals)) return 0;
  const bool sealed = (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) != 0;
  if (writable && sealed) return EPERM;
  // Linux clears VM_MAYWRITE for a shared mapping created after a write seal.
  *deny_write = sealed;
  if (!writable) return 0;
  if (write_lease == nullptr) return EINVAL;  // a writable mapping must hold its lease
  std::lock_guard<std::mutex> guard(g_seal_mutex);
  auto lease = std::make_shared<WriteLease>();
  if (const int error = lease->Acquire(file)) return error;
  // F_SEAL_WRITE cannot be added while the lease is held, so this read stays true.
  if (!ReadValue(file, &seals)) return EINVAL;
  if ((seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) != 0) return EPERM;  // the lease ends here
  *write_lease = std::move(lease);
  return 0;
}

void RenewMemfdWriteLeaseInChild(void* write_lease) {
  if (write_lease != nullptr) static_cast<WriteLease*>(write_lease)->RenewInChild();
}

int MemfdSharedMapError(int fd, bool writable, bool* deny_write,
                        std::shared_ptr<void>* write_lease) {
  HANDLE file = HandleFromFd(fd);
  RegisterMemfdIfSealed(fd, file);
  return MemfdSharedMapHandleError(file, writable, deny_write, write_lease);
}

}  // namespace gesher

#ifdef GESHER_FORK_LOCK
// fork (gesher/fork.h).
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::g_registry_mutex);
GESHER_FORK_LOCK(gesher::kForkOrderSubsystem, std::mutex, gesher::g_seal_mutex);
#endif
