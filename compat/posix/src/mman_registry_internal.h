// Private same-toolchain/STL ABI boundary; never a guest/public interface.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <map>
#include <memory>
#include <vector>
#include <array>
namespace gesher_mman_internal {
enum class SectionState { Borrowed, Live, Closed, ReleaseFailed, RollbackFailed };
struct SectionOwner {
  HANDLE handle = nullptr;
  size_t refs = 0;  // published views plus explicit staging pins, State::lock
  SectionState state = SectionState::Borrowed;
  DWORD error = 0;
  void* rollback_view = nullptr;
  SectionOwner* backing = nullptr;
  SectionOwner* next_failed = nullptr;
};
enum class Kind : uint8_t {
  kAnon,  // an allocation
  kView,  // a view of a file
  kHole,  // a placeholder (group regions only)
};

struct Region {
  size_t size = 0;            // bytes, multiple of the page size
  Kind kind = Kind::kAnon;
  // The placeholder reservation (its start) the region is a piece of; 0 for
  // a plain allocation or view, made without placeholders.
  uintptr_t group = 0;
  void* view = nullptr;       // views: what MapViewOfFile* returned
  HANDLE mapping = nullptr;   // owned legacy handle, borrowed if owner != nullptr
  SectionOwner* owner = nullptr;
  uint64_t offset = 0;        // group views: the file offset of the region's start
  ULONG view_protection = 0;  // group views: the protection they were mapped with
  // Slice A admission only: not the section maximum. Bounds certified by
  // this successfully created, whole logical view; invalidated by splits,
  // protection/locking changes. Each Region still owns its own section handle.
  bool shared_alias = false;
  size_t physical_extent = 0;
  size_t view_padding = 0;
  bool shared = false;        // views: MAP_SHARED
  std::vector<bool> live;     // one entry per page
  size_t live_pages = 0;
  std::vector<DWORD> actual_protection; // allocated only by promoted preparation
  DWORD operation_error = 0;
};


using RegionMap = std::map<uintptr_t, Region>;
struct AliasInput {
  uint64_t offset;
  size_t padding, length, page;
  ULONG protection;
  size_t source_length = 0; // zero means alias length in direct helper controls
};
struct PreparedAlias {
  RegionMap::node_type node;
  std::unique_ptr<SectionOwner> owner;
  std::vector<DWORD> source_protection; // detached, no OS ownership
};
enum class PrepareStatus { Ok, NoMemory, InvalidSize, InternalFailure };
PrepareStatus PrepareAlias(const AliasInput&, PreparedAlias&) noexcept;
PrepareStatus PrepareAliasFromNoExceptions(const AliasInput&, PreparedAlias&) noexcept; // dedicated CI caller object only
// Combined placeholder ledger prepared before common Punch transitions.
// This alone does not prepare per-piece/private constructor continuations.
struct PreparedPunchLedger { Region combined; };
PrepareStatus PreparePunchLedger(size_t, size_t, uintptr_t, PreparedPunchLedger&) noexcept;
PrepareStatus PreparePunchLedgerFromNoExceptions(size_t,size_t,uintptr_t,PreparedPunchLedger&) noexcept;
struct PreparedSplit { Region lower; RegionMap::node_type upper; };
PrepareStatus PrepareSplit(const Region&, uintptr_t, uintptr_t, size_t, PreparedSplit&) noexcept;
struct PreparedSurgery {
  std::array<RegionMap::node_type, 2> holes;
  std::array<Region, 2> mapped;
  std::vector<DWORD> desired_protection;
  std::vector<bool> desired_live;
};
PrepareStatus PrepareSurgery(const Region&, uintptr_t, uintptr_t, uintptr_t,
                             size_t, PreparedSurgery&) noexcept;
PrepareStatus PrepareSurgeryFromNoExceptions(const Region&, uintptr_t, uintptr_t, uintptr_t, size_t, PreparedSurgery&) noexcept;
// Request-wide PREPARATION DATA. Raw source handles are borrowed while the
// registry lock is held; executor must explicitly acquire/pin/retire them.
// No destructor closes a borrowed source handle or claims OS rollback.
struct PreparedPunchPiece {
  uintptr_t start=0,begin=0,end=0;
  Region original;
  std::array<RegionMap::node_type,3> partitions; // REAL placeholder states during transitions
  std::array<Region,2> kept; // publish only after actual retained view succeeds
  std::vector<DWORD> desired_protection;
  std::vector<uint8_t> private_bytes;
  std::array<std::unique_ptr<SectionOwner>,3> failure_records;
  std::array<HANDLE,2> retained_handles{};
};
struct PreparedPunchRequest {
  std::vector<PreparedPunchPiece> pieces;
  PreparedPunchLedger merged;
  PreparedPunchLedger cleanup_merged; // separate storage if constructor later fails
  size_t first_capacity=0,combined_pages=0;
};
PrepareStatus PreparePunchRequest(const RegionMap&,uintptr_t,uintptr_t,size_t,
                                 PreparedPunchRequest&) noexcept;
PrepareStatus PreparePunchRequestFromNoExceptions(const RegionMap&,uintptr_t,uintptr_t,
                                                size_t,PreparedPunchRequest&) noexcept;
}  // namespace gesher_mman_internal
