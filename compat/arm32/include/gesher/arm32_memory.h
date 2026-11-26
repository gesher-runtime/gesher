// gesher: checked, separately addressed memory for ARM32 guest execution.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

namespace gesher::arm32 {

// Guest addresses are values in a separate ILP32 address space, never truncated
// host pointers. The memory owner deliberately exposes no host backing pointer.
using GuestAddress = std::uint32_t;

enum class MemoryPermission : std::uint8_t {
  kNone = 0,
  kRead = 1,
  kWrite = 2,
  kExecute = 4,
};

constexpr MemoryPermission operator|(MemoryPermission a, MemoryPermission b) {
  return static_cast<MemoryPermission>(static_cast<std::uint8_t>(a) |
                                       static_cast<std::uint8_t>(b));
}
constexpr MemoryPermission operator&(MemoryPermission a, MemoryPermission b) {
  return static_cast<MemoryPermission>(static_cast<std::uint8_t>(a) &
                                       static_cast<std::uint8_t>(b));
}

class GuestMemory {
 public:
  static constexpr std::uint64_t kAddressLimit = std::uint64_t{1} << 32;
  static constexpr std::size_t kPageSize = 4096;
  static constexpr std::size_t kDefaultBudget = 256 * 1024 * 1024;

  explicit GuestMemory(std::size_t budget = kDefaultBudget);
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  // Mapping operations require a nonempty page-aligned range. The null page is
  // never mapped, and an end equal to 2^32 is valid. Map is zero-filled and never
  // replaces an existing page. Unmap and Protect require every page to exist.
  // Failed operations leave the complete address space unchanged, including on
  // allocation failure. The budget counts actual allocated guest backing bytes.
  bool Map(GuestAddress address, std::uint64_t size, MemoryPermission permissions,
           std::string* error = nullptr);
  bool Unmap(GuestAddress address, std::uint64_t size, std::string* error = nullptr);
  bool Protect(GuestAddress address, std::uint64_t size, MemoryPermission permissions,
               std::string* error = nullptr);

  // Transfers may span independently mapped adjacent pages. The complete range
  // is validated while holding the same lock used for the copy, so a fault never
  // produces a partial read or write. Empty transfers access no host buffer, but
  // still require an address outside the null page. Execute-only pages can be
  // fetched through ReadExecute without granting ordinary data reads.
  bool Read(GuestAddress address, void* destination, std::size_t size,
            std::string* error = nullptr) const;
  bool Write(GuestAddress address, const void* source, std::size_t size,
             std::string* error = nullptr);
  bool ReadExecute(GuestAddress address, void* destination, std::size_t size,
                   std::string* error = nullptr) const;
  bool IsMapped(GuestAddress address, std::uint64_t size,
                MemoryPermission permissions = MemoryPermission::kNone) const;

  // Explicit little-endian scalar access, including unaligned accesses. Output
  // values are unchanged on failure. Fetch methods require execute permission.
  bool Read8(GuestAddress address, std::uint8_t* value, std::string* error = nullptr) const;
  bool Read16(GuestAddress address, std::uint16_t* value, std::string* error = nullptr) const;
  bool Read32(GuestAddress address, std::uint32_t* value, std::string* error = nullptr) const;
  bool Read64(GuestAddress address, std::uint64_t* value, std::string* error = nullptr) const;
  bool Write8(GuestAddress address, std::uint8_t value, std::string* error = nullptr);
  bool Write16(GuestAddress address, std::uint16_t value, std::string* error = nullptr);
  bool Write32(GuestAddress address, std::uint32_t value, std::string* error = nullptr);
  bool Write64(GuestAddress address, std::uint64_t value, std::string* error = nullptr);
  bool Fetch16(GuestAddress address, std::uint16_t* value, std::string* error = nullptr) const;
  bool Fetch32(GuestAddress address, std::uint32_t* value, std::string* error = nullptr) const;

  // Atomically compare and conditionally replace one naturally aligned 1/2/4/8
  // byte little-endian value, synchronized with every other memory operation.
  // Returns false for invalid arguments or an access fault (output unchanged).
  // Otherwise returns true and sets exchanged, even when the comparison fails.
  // expected and desired must fit the selected width. This primitive is not an
  // ARM exclusive monitor: the translator must also maintain its reservations.
  bool CompareExchange(GuestAddress address, std::size_t width, std::uint64_t expected,
                       std::uint64_t desired, bool* exchanged, std::string* error = nullptr);

  std::size_t MappedBytes() const;
  std::size_t Budget() const { return budget_; }

 private:
  struct Page;
  bool CheckRange(GuestAddress address, std::uint64_t size, bool page_aligned,
                  std::string* error) const;
  bool CheckAccessLocked(GuestAddress address, std::uint64_t size,
                         MemoryPermission permissions, std::string* error) const;
  void CopyOutLocked(GuestAddress address, void* destination, std::size_t size) const;
  void CopyInLocked(GuestAddress address, const void* source, std::size_t size);
  bool ReadWithPermission(GuestAddress address, void* destination, std::size_t size,
                          MemoryPermission permission, std::string* error) const;

  const std::size_t budget_;
  mutable std::shared_mutex mutex_;
  std::map<GuestAddress, std::unique_ptr<Page>> pages_;
  std::size_t mapped_bytes_ = 0;
};

}  // namespace gesher::arm32
