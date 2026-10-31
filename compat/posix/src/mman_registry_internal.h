// Private same-toolchain/STL ABI boundary; never a guest/public interface.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <map>
#include <memory>
#include <vector>
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
};


using RegionMap = std::map<uintptr_t, Region>;
struct AliasInput {
  uint64_t offset;
  size_t padding, length, page;
  ULONG protection;
};
struct PreparedAlias {
  RegionMap::node_type node;
  std::unique_ptr<SectionOwner> owner;
};
enum class PrepareStatus { Ok, NoMemory, InvalidSize, InternalFailure };
PrepareStatus PrepareAlias(const AliasInput&, PreparedAlias&) noexcept;
}  // namespace gesher_mman_internal
