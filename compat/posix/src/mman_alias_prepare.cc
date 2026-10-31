// This TU alone enables exceptions; same compiler, STL, allocator and CRT.
#include "mman_registry_internal.h"
#include <limits>
#include <new>
#if !defined(__cpp_exceptions)
#error mman_alias_prepare must be compiled with exceptions enabled
#endif
namespace gesher_mman_internal {
PrepareStatus PrepareAlias(const AliasInput& in, PreparedAlias& out) noexcept {
  if (!out.node.empty() || out.owner) return PrepareStatus::InternalFailure;
  if (in.page == 0 || in.length == 0 || in.length % in.page != 0 ||
      in.length > std::numeric_limits<size_t>::max() - in.padding)
    return PrepareStatus::InvalidSize;
  try {
    Region alias;
    alias.kind = Kind::kView;
    alias.shared = alias.shared_alias = true;
    alias.offset = in.offset;
    alias.view_protection = in.protection;
    alias.view_padding = in.padding;
    alias.physical_extent = in.length + in.padding;
    alias.size = in.length;
    if (in.length / in.page > alias.live.max_size()) return PrepareStatus::InvalidSize;
    alias.live.assign(in.length / in.page, true);
    alias.live_pages = alias.live.size();
    RegionMap staging;
    staging.emplace(0, std::move(alias));
    auto owner = std::make_unique<SectionOwner>(); // no OS ownership yet
    out.node = staging.extract(staging.begin());
    out.owner = std::move(owner);
    return PrepareStatus::Ok;
  } catch (const std::bad_alloc&) {
    return PrepareStatus::NoMemory;
  } catch (...) {
    return PrepareStatus::InternalFailure;
  }
}
} // namespace gesher_mman_internal
