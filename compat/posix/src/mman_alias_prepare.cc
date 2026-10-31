// This TU alone enables exceptions; same compiler, STL, allocator and CRT.
#include "mman_registry_internal.h"
#include <limits>
#include <algorithm>
#include <new>
#if !defined(__cpp_exceptions)
#error mman_alias_prepare must be compiled with exceptions enabled
#endif
namespace gesher_mman_internal {
PrepareStatus PrepareAlias(const AliasInput& in, PreparedAlias& out) noexcept {
  if (!out.node.empty() || out.owner || !out.source_protection.empty()) return PrepareStatus::InternalFailure;
  if (in.page == 0 || in.length == 0 || in.length % in.page != 0 ||
      in.length > std::numeric_limits<size_t>::max() - in.padding)
    return PrepareStatus::InvalidSize;
  const size_t source_length=in.source_length?in.source_length:in.length;
  if(source_length<in.length || source_length%in.page) return PrepareStatus::InvalidSize;
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
    if(alias.live.size()>alias.actual_protection.max_size()) return PrepareStatus::InvalidSize;
    alias.actual_protection.assign(alias.live.size(),in.protection);
    // Source promotion must have its complete ledger before any view exists.
    std::vector<DWORD> source_protection;
    if(source_length/in.page>source_protection.max_size()) return PrepareStatus::InvalidSize;
    source_protection.assign(source_length/in.page,in.protection);
    RegionMap staging;
    staging.emplace(0, std::move(alias));
    auto owner = std::make_unique<SectionOwner>(); // no OS ownership yet
    out.node = staging.extract(staging.begin());
    out.owner = std::move(owner);
    out.source_protection = std::move(source_protection);
    return PrepareStatus::Ok;
  } catch (const std::bad_alloc&) {
    return PrepareStatus::NoMemory;
  } catch (...) {
    return PrepareStatus::InternalFailure;
  }
}

PrepareStatus PreparePunchLedger(size_t length,size_t page,uintptr_t group,
                                      PreparedPunchLedger& out) noexcept {
  if(!page || !length || length%page || !group || !out.combined.live.empty() ||
     !out.combined.actual_protection.empty()) return PrepareStatus::InvalidSize;
  try {
    Region ready; ready.kind=Kind::kHole; ready.group=group; ready.size=length;
    if(length/page>ready.live.max_size() || length/page>ready.actual_protection.max_size())
      return PrepareStatus::InvalidSize;
    ready.live.assign(length/page,false);
    ready.actual_protection.assign(length/page,0);
    out.combined=std::move(ready);
    return PrepareStatus::Ok;
  } catch(const std::bad_alloc&) { return PrepareStatus::NoMemory; }
    catch(...) { return PrepareStatus::InternalFailure; }
}

PrepareStatus PrepareSplit(const Region& source, uintptr_t start, uintptr_t at,
                           size_t page, PreparedSplit& out) noexcept {
  if (!out.upper.empty() || page == 0 || at <= start || at - start >= source.size ||
      (at - start) % page != 0 || source.size%page || source.live.size()!=source.size/page ||
      (!source.actual_protection.empty()&&source.actual_protection.size()!=source.live.size()))
    return PrepareStatus::InvalidSize;
  try {
    Region lower = source, upper = source; // snapshots BORROW, never ref increments
    size_t pages = (at - start) / page;
    upper.size = source.size - (at - start);
    upper.offset += at - start;
    upper.live.assign(source.live.begin()+pages, source.live.end());
    upper.live_pages = 0;
    for (bool live : upper.live) if (live) ++upper.live_pages;
    if (!source.actual_protection.empty()) upper.actual_protection.assign(
        source.actual_protection.begin()+pages,source.actual_protection.end());
    lower.size = at-start;
    lower.live.resize(pages);
    lower.live_pages -= upper.live_pages;
    if (!lower.actual_protection.empty()) lower.actual_protection.resize(pages);
    lower.shared_alias = upper.shared_alias = false;
    RegionMap nodes;
    nodes.emplace(at, std::move(upper));
    out.lower = std::move(lower);
    out.upper = nodes.extract(nodes.begin());
    return PrepareStatus::Ok;
  } catch (const std::bad_alloc&) { return PrepareStatus::NoMemory; }
    catch (...) { return PrepareStatus::InternalFailure; }
}

PrepareStatus PrepareSurgery(const Region& source, uintptr_t start, uintptr_t x,
                             uintptr_t y, size_t page, PreparedSurgery& out) noexcept {
  if (page == 0 || !source.owner || !source.shared || x < start || y <= x ||
      y-start > source.size || (x-start)%page || (y-start)%page ||
      source.size%page || source.live.size()!=source.size/page)
    return PrepareStatus::InvalidSize;
  try {
    PreparedSurgery work;
    work.desired_live = source.live;
    work.desired_protection.assign(source.size/page, PAGE_NOACCESS);
    const uintptr_t starts[2] = {start,y};
    const size_t lengths[2] = {x-start,source.size-(y-start)};
    for (size_t i=0;i<2;++i) {
      // Nodes for the two bounded actual placeholder splits, not for OS views.
      Region hole;
      hole.kind=Kind::kHole;
      hole.group=source.group;
      hole.size=i==0?x-start:y-x;
      hole.live.assign(hole.size/page,false);
      RegionMap staging;
      staging.emplace(i==0?start:x,std::move(hole));
      work.holes[i]=staging.extract(staging.begin());
      Region& mapped=work.mapped[i];
      mapped.kind=Kind::kView; mapped.group=source.group;
      mapped.mapping=source.mapping; mapped.owner=source.owner;
      mapped.shared=true; mapped.offset=source.offset+(starts[i]-start);
      mapped.view_protection=source.view_protection;
      mapped.size=lengths[i]; mapped.physical_extent=lengths[i];
      mapped.live.assign(lengths[i]/page,true);
      mapped.live_pages=mapped.live.size(); // entire actually created view
      mapped.actual_protection.assign(mapped.live.size(),source.view_protection);
    }
    out=std::move(work);
    return PrepareStatus::Ok;
  } catch (const std::bad_alloc&) { return PrepareStatus::NoMemory; }
    catch (...) { return PrepareStatus::InternalFailure; }
}
PrepareStatus PreparePunchRequest(const RegionMap& source,uintptr_t begin,uintptr_t end,
                                      size_t page,PreparedPunchRequest& out,size_t final_view) noexcept {
  if(!page || begin>=end || begin%page || end%page || !out.pieces.empty() ||
     !out.merged.combined.live.empty() || !out.cleanup_merged.combined.live.empty() ||
     !out.pins.empty()) return PrepareStatus::InvalidSize;
  auto first=source.upper_bound(begin);
  if(first==source.begin())return PrepareStatus::InvalidSize;
  --first;
  if(begin-first->first>=first->second.size || !first->second.group)
    return PrepareStatus::InvalidSize;
  try {
    PreparedPunchRequest work;
    work.first_capacity=first->second.live.capacity();
    work.combined_pages=(end-begin)/page;
    const auto merged=PreparePunchLedger(end-begin,page,first->second.group,work.merged);
    if(merged!=PrepareStatus::Ok)return merged;
    const auto cleanup=PreparePunchLedger(end-begin,page,first->second.group,work.cleanup_merged);
    if(cleanup!=PrepareStatus::Ok)return cleanup;
    const size_t length=end-begin;
    if(final_view==SIZE_MAX)final_view=length;
    if(final_view>length || final_view%page)return PrepareStatus::InvalidSize;
    work.constructor_first.kind=Kind::kHole;
    work.constructor_first.group=first->second.group;
    work.constructor_first.size=final_view?final_view:length;
    work.constructor_first.live.assign(work.constructor_first.size/page,false);
    work.constructor_first.actual_protection.assign(work.constructor_first.size/page,0);
    if(final_view && final_view<length) {
      Region tail;tail.kind=Kind::kHole;tail.group=first->second.group;tail.size=length-final_view;
      tail.live.assign(tail.size/page,false);tail.actual_protection.assign(tail.size/page,0);
      RegionMap detached;detached.emplace(begin+final_view,std::move(tail));
      work.constructor_tail=detached.extract(detached.begin());
    }
    for(auto& record:work.constructor_failures)record=std::make_unique<SectionOwner>();
    uintptr_t cursor=begin;
    for(auto at=first;cursor<end;++at) {
      if(at==source.end() || (at!=first && at->first!=cursor) || at->first>UINTPTR_MAX-at->second.size || at->first>cursor || cursor-at->first>=at->second.size ||
         at->second.group!=first->second.group || at->second.size%page ||
         at->second.live.size()!=at->second.size/page) return PrepareStatus::InvalidSize;
      const Region& original=at->second;
      PreparedPunchPiece piece;
      piece.start=at->first;piece.begin=cursor;
      piece.end=std::min(end,at->first+original.size);
      piece.original=original; // catches ALL vector snapshot allocation here
      piece.desired_protection.assign(original.size/page,0);
      if(original.kind==Kind::kView && !original.shared &&
         (piece.begin>at->first || piece.end<at->first+original.size))
        piece.private_bytes.resize(original.size); // fill only after all prep succeeds
      if(original.kind==Kind::kView && !original.owner)
        for(auto& record:piece.failure_records)record=std::make_unique<SectionOwner>();
      const uintptr_t boundaries[4]={at->first,piece.begin,piece.end,at->first+original.size};
      for(size_t part=0;part<3;++part) {
        const size_t length=boundaries[part+1]-boundaries[part];
        if(!length)continue;
        Region ready;
        const size_t offset=(boundaries[part]-at->first)/page,count=length/page;
        if(part!=1) {
          ready=original;
          ready.offset+=boundaries[part]-at->first;
          ready.shared_alias=false;
          ready.live.assign(original.live.begin()+offset,original.live.begin()+offset+count);
          ready.live_pages=static_cast<size_t>(std::count(ready.live.begin(),ready.live.end(),true));
          ready.actual_protection.assign(count,0); // populated by pre-retirement OS queries
        } else {
          ready.kind=Kind::kHole;ready.group=original.group;
          ready.live.assign(count,false);ready.actual_protection.assign(count,0);
        }
        ready.size=length;ready.physical_extent=length;ready.view_padding=0;
        if(part!=1) {
          piece.kept[part==0?0:1]=std::move(ready);
          ready=Region{};ready.kind=Kind::kHole;ready.group=original.group;ready.size=length;
          ready.live.assign(count,false);ready.actual_protection.assign(count,0);
        }
        RegionMap detached;
        detached.emplace(boundaries[part],std::move(ready));
        piece.partitions[part]=detached.extract(detached.begin());
      }
      if(original.owner) {
        if(original.kind!=Kind::kView || !original.shared ||
           original.owner->state!=SectionState::Live || !original.owner->refs)
          return PrepareStatus::InternalFailure;
        auto pin=std::find_if(work.pins.begin(),work.pins.end(),
          [&](const PreparedPunchPin& value){return value.owner==original.owner;});
        if(pin==work.pins.end()) {
          work.pins.push_back({original.owner,0,0});pin=std::prev(work.pins.end());
        }
        const size_t kept=(piece.start<piece.begin?1:0)+(piece.end<piece.start+original.size?1:0);
        const size_t maximum=std::numeric_limits<size_t>::max();
        if(pin->source_pins==maximum || pin->retained_views>maximum-kept)
          return PrepareStatus::InternalFailure;
        ++pin->source_pins;pin->retained_views+=kept;
        if(pin->source_pins>maximum-pin->retained_views ||
           original.owner->refs>maximum-pin->source_pins-pin->retained_views)
          return PrepareStatus::InternalFailure;
      }
      work.pieces.push_back(std::move(piece));
      cursor=work.pieces.back().end;
    }
    out=std::move(work);
    return PrepareStatus::Ok;
  } catch(const std::bad_alloc&) { return PrepareStatus::NoMemory; }
    catch(...) { return PrepareStatus::InternalFailure; }
}

} // namespace gesher_mman_internal
