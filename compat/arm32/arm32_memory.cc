// gesher: checked, separately addressed memory for ARM32 guest execution.
#include <gesher/arm32_memory.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

namespace gesher::arm32 {
namespace {

bool Fail(std::string* error, const char* message) {
  if (error != nullptr) {
    // A diagnostic must not turn a handled allocation failure into an exception.
    try {
      *error = message;
    } catch (const std::bad_alloc&) {
    }
  }
  return false;
}

bool ValidPermissions(MemoryPermission permissions) {
  return (static_cast<std::uint8_t>(permissions) & ~std::uint8_t{7}) == 0;
}

template <typename T>
bool ReadScalar(const GuestMemory& memory, GuestAddress address, T* value, bool execute,
                std::string* error) {
  if (value == nullptr) return Fail(error, "null scalar output");
  std::array<std::uint8_t, sizeof(T)> bytes{};
  if (!(execute ? memory.ReadExecute(address, bytes.data(), bytes.size(), error)
                : memory.Read(address, bytes.data(), bytes.size(), error))) return false;
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    result |= std::uint64_t{bytes[i]} << (i * 8);
  }
  *value = static_cast<T>(result);
  return true;
}

template <typename T>
bool WriteScalar(GuestMemory& memory, GuestAddress address, T value, std::string* error) {
  std::array<std::uint8_t, sizeof(T)> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (i * 8));
  }
  return memory.Write(address, bytes.data(), bytes.size(), error);
}

}  // namespace

struct GuestMemory::Page {
  std::array<std::uint8_t, kPageSize> bytes{};
  MemoryPermission permissions = MemoryPermission::kNone;
};

GuestMemory::GuestMemory(std::size_t budget) : budget_(budget) {}
GuestMemory::~GuestMemory() = default;

bool GuestMemory::CheckRange(GuestAddress address, std::uint64_t size, bool page_aligned,
                             std::string* error) const {
  if (address < kPageSize) return Fail(error, "access to the ARM32 null page");
  if (size > kAddressLimit - std::uint64_t{address}) {
    return Fail(error, "ARM32 address range overflows 4 GiB");
  }
  if (page_aligned && (size == 0 || address % kPageSize != 0 || size % kPageSize != 0)) {
    return Fail(error, "mapping range must be nonempty and page aligned");
  }
  return true;
}

bool GuestMemory::CheckAccessLocked(GuestAddress address, std::uint64_t size,
                                    MemoryPermission permissions, std::string* error) const {
  if (!CheckRange(address, size, false, error)) return false;
  if (!ValidPermissions(permissions)) return Fail(error, "invalid memory permissions");
  const auto required = static_cast<std::uint8_t>(permissions);
  const std::uint64_t end = std::uint64_t{address} + size;
  if (size == 0) return true;
  for (std::uint64_t page = address - address % kPageSize; page < end; page += kPageSize) {
    auto found = pages_.find(static_cast<GuestAddress>(page));
    if (found == pages_.end()) return Fail(error, "unmapped ARM32 memory");
    if ((static_cast<std::uint8_t>(found->second->permissions) & required) != required) {
      return Fail(error, "ARM32 memory permission denied");
    }
  }
  return true;
}

bool GuestMemory::Map(GuestAddress address, std::uint64_t size, MemoryPermission permissions,
                      std::string* error) {
  if (!CheckRange(address, size, true, error)) return false;
  if (!ValidPermissions(permissions)) return Fail(error, "invalid memory permissions");
  std::unique_lock lock(mutex_);
  if (size > budget_ - mapped_bytes_) return Fail(error, "ARM32 memory budget exceeded");
  const std::uint64_t end = std::uint64_t{address} + size;
  auto existing = pages_.lower_bound(address);
  if (existing != pages_.end() && existing->first < end) {
    return Fail(error, "ARM32 mapping overlaps an existing page");
  }
  // A separate map owns every allocation until staging succeeds. Merging equal
  // std::map allocators transfers nodes, so publishing needs no new allocation.
  std::map<GuestAddress, std::unique_ptr<Page>> staged;
  try {
    for (std::uint64_t at = address; at < end; at += kPageSize) {
      auto page = std::make_unique<Page>();
      page->permissions = permissions;
      staged.emplace(static_cast<GuestAddress>(at), std::move(page));
    }
  } catch (const std::bad_alloc&) {
    return Fail(error, "cannot allocate ARM32 memory");
  }
  pages_.merge(staged);
  mapped_bytes_ += static_cast<std::size_t>(size);
  return true;
}

bool GuestMemory::Unmap(GuestAddress address, std::uint64_t size, std::string* error) {
  if (!CheckRange(address, size, true, error)) return false;
  std::unique_lock lock(mutex_);
  if (!CheckAccessLocked(address, size, MemoryPermission::kNone, error)) return false;
  const std::uint64_t end = std::uint64_t{address} + size;
  auto first = pages_.lower_bound(address);
  auto last = end == kAddressLimit ? pages_.end()
                                  : pages_.lower_bound(static_cast<GuestAddress>(end));
  pages_.erase(first, last);
  mapped_bytes_ -= static_cast<std::size_t>(size);
  return true;
}

bool GuestMemory::Protect(GuestAddress address, std::uint64_t size, MemoryPermission permissions,
                          std::string* error) {
  if (!CheckRange(address, size, true, error)) return false;
  if (!ValidPermissions(permissions)) return Fail(error, "invalid memory permissions");
  std::unique_lock lock(mutex_);
  if (!CheckAccessLocked(address, size, MemoryPermission::kNone, error)) return false;
  const std::uint64_t end = std::uint64_t{address} + size;
  for (auto page = pages_.lower_bound(address); page != pages_.end() && page->first < end; ++page) {
    page->second->permissions = permissions;
  }
  return true;
}

void GuestMemory::CopyOutLocked(GuestAddress address, void* destination, std::size_t size) const {
  auto* output = static_cast<std::uint8_t*>(destination);
  std::uint64_t at = address;
  while (size != 0) {
    const auto offset = static_cast<std::size_t>(at % kPageSize);
    const auto count = std::min(size, kPageSize - offset);
    const auto& page = pages_.find(static_cast<GuestAddress>(at - offset))->second;
    std::memcpy(output, page->bytes.data() + offset, count);
    output += count;
    at += count;
    size -= count;
  }
}

void GuestMemory::CopyInLocked(GuestAddress address, const void* source, std::size_t size) {
  const auto* input = static_cast<const std::uint8_t*>(source);
  std::uint64_t at = address;
  while (size != 0) {
    const auto offset = static_cast<std::size_t>(at % kPageSize);
    const auto count = std::min(size, kPageSize - offset);
    auto& page = pages_.find(static_cast<GuestAddress>(at - offset))->second;
    std::memcpy(page->bytes.data() + offset, input, count);
    input += count;
    at += count;
    size -= count;
  }
}

bool GuestMemory::ReadWithPermission(GuestAddress address, void* destination, std::size_t size,
                                     MemoryPermission permission, std::string* error) const {
  if (destination == nullptr && size != 0) return Fail(error, "null read destination");
  std::shared_lock lock(mutex_);
  if (!CheckAccessLocked(address, size, permission, error)) return false;
  CopyOutLocked(address, destination, size);
  return true;
}

bool GuestMemory::Read(GuestAddress address, void* destination, std::size_t size,
                       std::string* error) const {
  return ReadWithPermission(address, destination, size, MemoryPermission::kRead, error);
}

bool GuestMemory::ReadExecute(GuestAddress address, void* destination, std::size_t size,
                              std::string* error) const {
  return ReadWithPermission(address, destination, size, MemoryPermission::kExecute, error);
}

bool GuestMemory::Write(GuestAddress address, const void* source, std::size_t size,
                        std::string* error) {
  if (source == nullptr && size != 0) return Fail(error, "null write source");
  std::unique_lock lock(mutex_);
  if (!CheckAccessLocked(address, size, MemoryPermission::kWrite, error)) return false;
  CopyInLocked(address, source, size);
  return true;
}

bool GuestMemory::IsMapped(GuestAddress address, std::uint64_t size,
                           MemoryPermission permissions) const {
  std::shared_lock lock(mutex_);
  return CheckAccessLocked(address, size, permissions, nullptr);
}

bool GuestMemory::Read8(GuestAddress address, std::uint8_t* value, std::string* error) const {
  return ReadScalar(*this, address, value, false, error);
}
bool GuestMemory::Read16(GuestAddress address, std::uint16_t* value, std::string* error) const {
  return ReadScalar(*this, address, value, false, error);
}
bool GuestMemory::Read32(GuestAddress address, std::uint32_t* value, std::string* error) const {
  return ReadScalar(*this, address, value, false, error);
}
bool GuestMemory::Read64(GuestAddress address, std::uint64_t* value, std::string* error) const {
  return ReadScalar(*this, address, value, false, error);
}
bool GuestMemory::Write8(GuestAddress address, std::uint8_t value, std::string* error) {
  return WriteScalar(*this, address, value, error);
}
bool GuestMemory::Write16(GuestAddress address, std::uint16_t value, std::string* error) {
  return WriteScalar(*this, address, value, error);
}
bool GuestMemory::Write32(GuestAddress address, std::uint32_t value, std::string* error) {
  return WriteScalar(*this, address, value, error);
}
bool GuestMemory::Write64(GuestAddress address, std::uint64_t value, std::string* error) {
  return WriteScalar(*this, address, value, error);
}
bool GuestMemory::Fetch16(GuestAddress address, std::uint16_t* value, std::string* error) const {
  return ReadScalar(*this, address, value, true, error);
}
bool GuestMemory::Fetch32(GuestAddress address, std::uint32_t* value, std::string* error) const {
  return ReadScalar(*this, address, value, true, error);
}

bool GuestMemory::CompareExchange(GuestAddress address, std::size_t width, std::uint64_t expected,
                                  std::uint64_t desired, bool* exchanged, std::string* error) {
  if (exchanged == nullptr) return Fail(error, "null compare-exchange output");
  if (width != 1 && width != 2 && width != 4 && width != 8) {
    return Fail(error, "invalid compare-exchange width");
  }
  if (address % width != 0) return Fail(error, "unaligned compare-exchange");
  if (width < 8 && ((expected | desired) >> (width * 8)) != 0) {
    return Fail(error, "compare-exchange value exceeds its width");
  }
  std::unique_lock lock(mutex_);
  if (!CheckAccessLocked(address, width, MemoryPermission::kRead | MemoryPermission::kWrite,
                         error)) return false;
  std::array<std::uint8_t, 8> bytes{};
  CopyOutLocked(address, bytes.data(), width);
  std::uint64_t observed = 0;
  for (std::size_t i = 0; i < width; ++i) observed |= std::uint64_t{bytes[i]} << (i * 8);
  if (observed != expected) {
    *exchanged = false;
    return true;
  }
  for (std::size_t i = 0; i < width; ++i) {
    bytes[i] = static_cast<std::uint8_t>(desired >> (i * 8));
  }
  CopyInLocked(address, bytes.data(), width);
  *exchanged = true;
  return true;
}

std::size_t GuestMemory::MappedBytes() const {
  std::shared_lock lock(mutex_);
  return mapped_bytes_;
}

}  // namespace gesher::arm32
