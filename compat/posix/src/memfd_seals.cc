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
// mmap/mprotect (mman.cc). A descriptor is known to be a memfd by a registry
// (memfd_create, Binder receive, dup), so other files pay one atomic load.
//
// F_SEAL_WRITE requires that no writable shared mapping of the file exists.
// Mappings of other processes cannot be counted here: once any process has
// mapped the file writable and shared, F_SEAL_WRITE answers EBUSY (as Linux
// does while such a mapping exists), even after it is unmapped.
// F_SEAL_FUTURE_WRITE, which Android's ashmem-over-memfd uses, has no such
// condition.
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "internal.h"
#include "record_lock_liveness.h"

namespace gesher {
namespace {

constexpr char kSealsName[] = "GESHER.SEALS";
constexpr size_t kSealsNameLength = sizeof(kSealsName) - 1;
// Internal, never reported: the file has had a writable shared mapping.
constexpr uint32_t kWritablyMapped = 0x80000000u;
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

// Seals only grow. Another process may add seals at the same time: write,
// read back, and repeat until the stored value holds every bit asked for.
bool OrValue(HANDLE file, uint32_t bits, uint32_t* result) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    uint32_t current = 0;
    if (!ReadValue(file, &current)) return errno = EINVAL, false;
    if ((current & bits) == bits) {
      if (result != nullptr) *result = current;
      return true;
    }
    if (!WriteValue(file, current | bits)) return false;
  }
  return errno = EAGAIN, false;
}

std::mutex g_registry_mutex;
std::unordered_set<int> g_registry;
std::atomic<size_t> g_registry_count{0};

}  // namespace

bool InitMemfdSeals(HANDLE file, bool allow_sealing) {
  // Without MFD_ALLOW_SEALING, Linux reports F_SEAL_SEAL: no seal can be added.
  return WriteValue(file, allow_sealing ? 0u : static_cast<uint32_t>(F_SEAL_SEAL));
}

void RegisterMemfd(int fd) {
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (g_registry.insert(fd).second) g_registry_count.fetch_add(1, std::memory_order_release);
}

void ForgetMemfd(int fd) {
  if (g_registry_count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (g_registry.erase(fd) != 0) g_registry_count.fetch_sub(1, std::memory_order_release);
}

bool IsRegisteredMemfd(int fd) {
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
  if ((seals & F_SEAL_WRITE) != 0 && (current & F_SEAL_WRITE) == 0 &&
      (current & kWritablyMapped) != 0) {
    return errno = EBUSY, -1;
  }
  return OrValue(file, seals, nullptr) ? 0 : -1;
}

int MemfdWriteError(int fd, bool at_position, uint64_t offset, uint64_t length) {
  if (!IsRegisteredMemfd(fd)) return 0;
  HANDLE file = HandleFromFd(fd);
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals)) return 0;
  if ((seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) != 0) return EPERM;
  if ((seals & F_SEAL_GROW) != 0 && length != 0) {
    if (at_position) {
      const __int64 position = _telli64(fd);
      if (position < 0) return 0;  // the write itself reports the descriptor's error
      offset = static_cast<uint64_t>(position);
    }
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && offset + length > static_cast<uint64_t>(size.QuadPart)) {
      return EPERM;
    }
  }
  return 0;
}

int MemfdResizeError(int fd, uint64_t new_size) {
  if (!IsRegisteredMemfd(fd)) return 0;
  HANDLE file = HandleFromFd(fd);
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals)) return 0;
  if ((seals & (F_SEAL_GROW | F_SEAL_SHRINK)) == 0) return 0;
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size)) return 0;
  const uint64_t current = static_cast<uint64_t>(size.QuadPart);
  if (new_size > current && (seals & F_SEAL_GROW) != 0) return EPERM;
  if (new_size < current && (seals & F_SEAL_SHRINK) != 0) return EPERM;
  return 0;
}

int MemfdGrowError(int fd, uint64_t end) {
  if (!IsRegisteredMemfd(fd)) return 0;
  HANDLE file = HandleFromFd(fd);
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals) || (seals & F_SEAL_GROW) == 0) return 0;
  LARGE_INTEGER size;
  return GetFileSizeEx(file, &size) && end > static_cast<uint64_t>(size.QuadPart) ? EPERM : 0;
}

int MemfdSharedMapError(int fd, bool writable, bool* deny_write) {
  *deny_write = false;
  HANDLE file = HandleFromFd(fd);
  uint32_t seals = 0;
  if (file == INVALID_HANDLE_VALUE || !ReadValue(file, &seals)) return 0;  // not a memfd
  RegisterMemfd(fd);
  const bool sealed = (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) != 0;
  if (writable && sealed) return EPERM;
  // A shared mapping made after a write seal can never become writable
  // (Linux clears its VM_MAYWRITE).
  *deny_write = sealed;
  if (writable && (seals & kWritablyMapped) == 0 && !OrValue(file, kWritablyMapped, nullptr)) {
    return errno;
  }
  return 0;
}

}  // namespace gesher
