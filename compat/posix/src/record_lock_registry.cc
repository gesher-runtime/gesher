// Versioned cross-session metadata with checked fixed-size transactions.
// The all-users metadata is advisory: no mandatory lock covers user file data.
#include "record_lock_registry.h"
#include "internal.h"
#include <sddl.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <iterator>
#include <new>
#include <type_traits>

namespace gesher::record_locks {
namespace {

constexpr LONG64 kMagic = INT64_C(0x476573684C6F636B);
constexpr uint32_t kVersion = 1;
struct alignas(64) Header {
  volatile LONG64 magic;
  uint32_t version, state_bytes;
  volatile LONG active;
  uint32_t reserved[11];
};
static_assert(sizeof(Header) == 64);
struct Bank { uint64_t generation, crc; RegistryState state; };
struct Disk { Header header; Bank banks[2]; };
static_assert(std::is_trivially_copyable_v<RegistryState>);
static_assert(sizeof(Disk) < UINT32_MAX);

uint64_t Crc(const void* data, size_t size, uint64_t crc = 0) {
  static constexpr auto table = [] {
    std::array<uint64_t, 256> result{};
    for (uint64_t index = 0; index < 256; ++index) {
      uint64_t value = index << 56;
      for (unsigned bit = 0; bit < 8; ++bit) {
        value = (value << 1) ^ ((value >> 63) ? UINT64_C(0x42f0e1eba9ea3693) : 0);
      }
      result[index] = value;
    }
    return result;
  }();
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t index = 0; index < size; ++index) crc = (crc << 8) ^ table[(crc >> 56) ^ bytes[index]];
  return crc;
}

uint64_t BankCrc(const Bank& bank) {
  return Crc(&bank.state, sizeof(bank.state), Crc(&bank.generation, sizeof(bank.generation)));
}

bool ValidOwner(const Owner& owner) {
  return static_cast<unsigned>(owner.kind) <= 1 && (owner.low != 0 || owner.high != 0);
}

bool ValidRecord(const Record& record) {
  return ValidOwner(record.owner) && static_cast<unsigned>(record.kind) < 2 &&
    record.range.start < record.range.end && record.range.end <= kThroughEof &&
    (record.owner.kind == Ownership::kDescription ? record.pid == -1 :
      record.pid > 0 && record.owner.low == static_cast<uint32_t>(record.pid) && record.owner.high != 0);
}

bool ValidState(const RegistryState& state) {
  if (state.locks.count > kRecordCapacity || state.reference_count > kReferenceCapacity ||
      state.wait_count > kWaitCapacity) return false;
  for (size_t index = 0; index < state.reference_count; ++index) {
    const Reference& reference = state.references[index];
    if (!ValidOwner(reference.description) || reference.description.kind != Ownership::kDescription ||
        reference.process.pid == 0 || reference.process.created == 0 || reference.native_handle == 0 ||
        reference.native_handle == UINT64_MAX) return false;
  }
  for (const Record& record : state.locks) {
    if (!ValidRecord(record)) return false;
    if (record.owner.kind == Ownership::kDescription) {
      bool referenced = false;
      for (size_t index = 0; index < state.reference_count; ++index) {
        if (state.references[index].description == record.owner && state.references[index].file == record.file) {
          referenced = true;
          break;
        }
      }
      if (!referenced) return false;
    }
  }
  for (size_t index = 0; index < state.wait_count; ++index) {
    const WaitingRequest& request = state.waits[index];
    if (!ValidRecord(request.requested) || request.thread.process.pid == 0 || request.thread.process.created == 0 ||
        request.thread.tid == 0 || request.thread.created == 0 || (request.token_low == 0 && request.token_high == 0)) return false;
  }
  return true;
}

int WinError() { return gesher::ErrnoFromWin32(GetLastError()); }

// Shared metadata has the same secured global location in standalone and
// installed contexts, independent of environment overrides/session IDs. A
// failed namespace/ACL operation is an error, never a per-session fallback.
int MetadataPath(wchar_t* path, size_t count, SECURITY_ATTRIBUTES* security) {
  PWSTR program_data = nullptr;
  HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data);
  if (FAILED(result)) return EACCES;
  int written = std::swprintf(path, count, L"%ls\\Gesher", program_data);
  CoTaskMemFree(program_data);
  if (written < 0 || static_cast<size_t>(written) >= count) return ENAMETOOLONG;
  if (!CreateDirectoryW(path, security) && GetLastError() != ERROR_ALREADY_EXISTS) return WinError();
  size_t length = std::wcslen(path);
  written = std::swprintf(path + length, count - length, L"\\RecordLocks-v1");
  if (written < 0 || static_cast<size_t>(written) >= count - length) return ENAMETOOLONG;
  if (!CreateDirectoryW(path, security) && GetLastError() != ERROR_ALREADY_EXISTS) return WinError();
  length = std::wcslen(path);
  written = std::swprintf(path + length, count - length, L"\\registry.bin");
  return written < 0 || static_cast<size_t>(written) >= count - length ? ENAMETOOLONG : 0;
}

}  // namespace

Registry::~Registry() { Reset(); }

void Registry::Unlock() {
  if (held_) {
    ReleaseMutex(mutex_);
    held_ = false;
  }
  active_ = -1;
  generation_ = 0;
}

void Registry::Reset() {
  Unlock();
  if (disk_) UnmapViewOfFile(disk_);
  if (mapping_) CloseHandle(mapping_);
  if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  if (mutex_) CloseHandle(mutex_);
  if (staged_) {
    if (release_) release_(staged_);
    else HeapFree(GetProcessHeap(), 0, staged_);
  }
  disk_ = nullptr;
  mapping_ = mutex_ = nullptr;
  file_ = INVALID_HANDLE_VALUE;
  staged_ = nullptr;
}

int Registry::Open() {
  if (disk_) return 0;
  if ((allocate_ == nullptr) != (release_ == nullptr)) return EINVAL;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)", SDDL_REVISION_1, &descriptor, nullptr)) return WinError();
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  wchar_t path[32768];
  int error = MetadataPath(path, std::size(path), &security);
  if (!error) {
    mutex_ = CreateMutexW(&security, FALSE, L"Global\\Gesher.RecordLocks.v1");
    if (!mutex_) error = WinError();
  }
  if (!error) {
    DWORD wait = WaitForSingleObject(mutex_, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) error = wait == WAIT_FAILED ? WinError() : EIO;
    else held_ = true;
  }
  if (!error) {
    file_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) error = WinError();
  }
  LocalFree(descriptor);
  if (!error) {
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file_, &info) || !GetFileSizeEx(file_, &size)) error = WinError();
    else if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) error = ELOOP;
    else if (size.QuadPart == 0) {
      LARGE_INTEGER required{};
      required.QuadPart = sizeof(Disk);
      if (!SetFilePointerEx(file_, required, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) error = WinError();
    } else if (static_cast<uint64_t>(size.QuadPart) != sizeof(Disk)) error = EPROTO;
  }
  if (!error) {
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, sizeof(Disk), nullptr);
    if (!mapping_) error = WinError();
  }
  if (!error) {
    disk_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Disk));
    if (!disk_) error = WinError();
  }
  if (!error) {
    auto* disk = static_cast<Disk*>(disk_);
    if (InterlockedCompareExchange64(&disk->header.magic, 0, 0) == 0 && disk->header.version == 0) {
      // A committed registry never clears magic. Only a new/unpublished
      // initialization may enter here; initialized corruption is not reset.
      std::memset(static_cast<void*>(disk), 0, sizeof(*disk));
      disk->banks[0].generation = 1;
      disk->banks[0].crc = BankCrc(disk->banks[0]);
      disk->header.version = kVersion;
      disk->header.state_bytes = sizeof(RegistryState);
      InterlockedExchange(&disk->header.active, 0);
      MemoryBarrier();
      InterlockedExchange64(&disk->header.magic, kMagic);
    }
    error = Validate();
  }
  if (!error) {
    void* storage = allocate_ ? allocate_(sizeof(RegistryState)) :
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RegistryState));
    if (!storage) error = ENOMEM;
    else staged_ = new (storage) RegistryState{};
  }
  if (error) Reset();
  else Unlock();
  return error;
}

int Registry::Validate() {
  auto* disk = static_cast<Disk*>(disk_);
  if (InterlockedCompareExchange64(&disk->header.magic, 0, 0) != kMagic ||
      disk->header.version != kVersion || disk->header.state_bytes != sizeof(RegistryState)) return EPROTO;
  LONG active = InterlockedCompareExchange(&disk->header.active, 0, 0);
  if (active != 0 && active != 1) return EIO;
  const Bank& bank = disk->banks[active];
  if (bank.generation == 0 || BankCrc(bank) != bank.crc || !ValidState(bank.state)) return EIO;
  active_ = active;
  generation_ = bank.generation;
  return 0;
}

int Registry::Begin(RegistryState** state) {
  if (!disk_ || !staged_ || held_ || state == nullptr) return EINVAL;
  DWORD wait = WaitForSingleObject(mutex_, INFINITE);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return wait == WAIT_FAILED ? WinError() : EIO;
  held_ = true;
  int error = Validate();
  if (error) { Unlock(); return error; }
  auto* disk = static_cast<Disk*>(disk_);
  std::memcpy(staged_, &disk->banks[active_].state, sizeof(*staged_));
  *state = staged_;
  return 0;
}

int Registry::Commit() {
  if (!held_ || active_ < 0 || !staged_) return EINVAL;
  if (!ValidState(*staged_)) return EINVAL;
  if (generation_ == UINT64_MAX) return EOVERFLOW;
  auto* disk = static_cast<Disk*>(disk_);
  Bank& next = disk->banks[1 - active_];
  next.generation = generation_ + 1;
  std::memcpy(&next.state, staged_, sizeof(next.state));
  next.crc = BankCrc(next);
  // Every byte/checksum of the inactive bank precedes the sole publication
  // point. An abandoned writer before this exchange leaves the old active
  // bank authoritative. No fallback accepts an invalid active bank.
  MemoryBarrier();
  InterlockedExchange(&disk->header.active, 1 - active_);
  active_ = 1 - active_;
  generation_ = next.generation;
  return 0;
}

}  // namespace gesher::record_locks
