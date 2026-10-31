// CI-only pinned helper/allocator qualification; build with same exception
// runtime/ABI and source-local helper options. No synthetic NO_MEMORY return.
#include "../../compat/posix/src/mman_registry_internal.h"
#include <cstdlib>
#include <cstdio>
#include <new>
using namespace gesher_mman_internal;
namespace { long budget = -1; long live = 0; long calls = 0; long injected = 0; }
void* operator new(std::size_t size) {
  ++calls;
  if (budget == 0) { ++injected; throw std::bad_alloc(); }
  if (budget > 0) --budget;
  void* result = std::malloc(size ? size : 1);
  if (!result) throw std::bad_alloc();
  ++live;
  return result;
}
void operator delete(void* ptr) noexcept { if (ptr) { --live; std::free(ptr); } }
void operator delete(void* ptr, std::size_t) noexcept { ::operator delete(ptr); }
int main() {
  int failed = 0, failures = 0, successes = 0;
  for (long point = 0; point < 16; ++point) {
    PrepareStatus status;
    bool empty;
    const long initial = live;
    budget = point;
    calls = 0; injected=0;
    {
      PreparedAlias out;
      status = PrepareAliasFromNoExceptions({0, 0, 4096, 4096, PAGE_READWRITE,5*4096}, out);
      empty = out.node.empty() && !out.owner && out.source_protection.empty();
      budget = -1;
      if (status == PrepareStatus::NoMemory) { ++failures; if (!empty) ++failed; }
      else if (status == PrepareStatus::Ok) {
        ++successes;
        if (out.node.empty() || !out.owner || out.owner->state != SectionState::Borrowed ||
            out.owner->handle != nullptr || out.node.mapped().live_pages != 1 ||
            out.node.mapped().actual_protection.size()!=1 || out.node.mapped().actual_protection[0]!=PAGE_READWRITE ||
            out.source_protection.size()!=5 || out.source_protection[0]!=PAGE_READWRITE || out.source_protection[4]!=PAGE_READWRITE) ++failed;
      } else ++failed;
    }
    const bool released = live == initial;
    if(injected!=(status==PrepareStatus::NoMemory?1:0))++failed;
    std::printf("{\"event\":\"allocation\",\"family\":\"alias\",\"budget\":%ld,\"allocation_calls\":%ld,\"injected\":%ld,\"status\":\"%s\",\"outstanding\":%ld,\"unchanged_owner_refs\":%s}\n",
      point,calls,injected,status==PrepareStatus::Ok?"OK":status==PrepareStatus::NoMemory?"NO_MEMORY":"ERROR",live-initial,released?"true":"false");
    if (!released) ++failed;
    std::printf("PREPARE_ALLOCATION point=%ld calls=%ld status=%d empty=%d released=%d\n",
                point, calls, static_cast<int>(status), empty, released);
    if (successes) break;
  }
  budget = -1;
  PreparedAlias out;
  if (PrepareAliasFromNoExceptions({0, 0, 0, 4096, PAGE_READWRITE}, out) != PrepareStatus::InvalidSize ||
      !out.node.empty() || out.owner || !out.source_protection.empty() || failures < 3 || successes != 1) ++failed;
  {
    SectionOwner owner; owner.state=SectionState::Live; owner.refs=3;
    Region source; source.kind=Kind::kView; source.shared=true; source.owner=&owner;
    source.group=0x10000; source.size=5*4096; source.view_protection=PAGE_READWRITE;
    source.live.assign(5,true); source.live_pages=5;
    source.actual_protection.assign(5,PAGE_READWRITE);
    bool surgery_success=false; int surgery_failures=0;
    for(long point=0;point<64;++point) {
      long before=live; budget=point; calls=0; injected=0;
      PrepareStatus status;
      {
        PreparedSurgery plan;
        status=PrepareSurgeryFromNoExceptions(source,0x10000,0x12000,0x13000,4096,plan);
        budget=-1;
        if(status==PrepareStatus::NoMemory) {
          ++surgery_failures;
          if(!plan.holes[0].empty() || !plan.holes[1].empty() || !plan.desired_live.empty() ||
             !plan.desired_protection.empty())++failed;
        } else if(status==PrepareStatus::Ok) {
          surgery_success=true;
          if(plan.mapped[0].live_pages!=2 || plan.mapped[1].live_pages!=2 ||
             plan.desired_live.size()!=5 || plan.desired_protection.size()!=5)++failed;
        } else ++failed;
      }
      if(live!=before || owner.refs!=3 || injected!=(status==PrepareStatus::NoMemory?1:0))++failed;
      std::printf("{\"event\":\"allocation\",\"family\":\"surgery\",\"budget\":%ld,\"allocation_calls\":%ld,\"injected\":%ld,\"status\":\"%s\",\"outstanding\":%ld,\"unchanged_owner_refs\":%s}\n",
        point,calls,injected,status==PrepareStatus::Ok?"OK":status==PrepareStatus::NoMemory?"NO_MEMORY":"ERROR",live-before,owner.refs==3?"true":"false");
      std::printf("SURGERY_ALLOCATION point=%ld calls=%ld status=%d released=%d refs=%zu\n",
                  point,calls,static_cast<int>(status),live==before,owner.refs);
      if(surgery_success)break;
    }
    if(!surgery_success || surgery_failures<3)++failed;
  }
  {
    // Positively choose growth beyond the ACTUAL first bitmap capacity, not
    // a hardcoded five-page assumption. Native execution remains CI-only.
    std::vector<bool> first(128,false);
    const size_t capacity=first.capacity(),pages=capacity+1;
    bool success=false;int caught=0;
    if(pages<=capacity || pages>SIZE_MAX/4096)++failed;
    else for(long point=0;point<32;++point) {
      const long before=live;budget=point;calls=0;injected=0;
      PrepareStatus status;
      {
        PreparedPunchLedger plan;
        status=PreparePunchLedgerFromNoExceptions(pages*4096,4096,0x10000,plan);
        budget=-1;
        if(status==PrepareStatus::NoMemory) {
          ++caught;
          if(!plan.combined.live.empty()||!plan.combined.actual_protection.empty())++failed;
        } else if(status==PrepareStatus::Ok) {
          success=true;
          if(plan.combined.live.size()!=pages || plan.combined.live_pages ||
             plan.combined.actual_protection.size()!=pages || plan.combined.owner)++failed;
        } else ++failed;
      }
      if(live!=before || injected!=(status==PrepareStatus::NoMemory?1:0))++failed;
      std::printf("PUNCH_LEDGER_ALLOCATION capacity=%zu combined_pages=%zu point=%ld calls=%ld injected=%ld status=%d released=%d\n",
        capacity,pages,point,calls,injected,static_cast<int>(status),live==before);
      if(success)break;
    }
    if(!success||!caught)++failed;
  }
  {
    SectionOwner source_owner;source_owner.state=SectionState::Live;source_owner.refs=2;
    RegionMap source;
    Region prefix;prefix.kind=Kind::kView;prefix.group=0x100000;
    prefix.size=128*4096;prefix.shared=true;prefix.owner=&source_owner;prefix.live.assign(128,true);prefix.live_pages=128;prefix.actual_protection.assign(128,PAGE_READWRITE);
    source.emplace(0x100000,std::move(prefix));
    Region middle;middle.kind=Kind::kAnon;middle.group=0x100000;
    const size_t middle_pages=source.begin()->second.live.capacity()+16;
    middle.size=middle_pages*4096;middle.live.assign(middle_pages,true);middle.live_pages=middle_pages;
    const uintptr_t suffix_start=0x180000+middle.size;
    source.emplace(0x180000,std::move(middle));
    Region suffix;suffix.kind=Kind::kView;suffix.group=0x100000;
    suffix.size=128*4096;suffix.live.assign(128,true);suffix.live_pages=128;
    source.emplace(suffix_start,std::move(suffix)); // private continuation, partial edge
    Region alias;alias.kind=Kind::kView;alias.shared=true;alias.owner=&source_owner;
    alias.group=0x400000;alias.size=4096;alias.live.assign(1,true);alias.live_pages=1;
    alias.actual_protection.assign(1,PAGE_READWRITE);source.emplace(0x400000,std::move(alias));
    bool success=false;int caught=0;
    for(long point=0;point<256;++point) {
      const long before=live;calls=0;injected=0;budget=point;
      PrepareStatus status;
      {
        PreparedPunchRequest plan;
        status=PreparePunchRequestFromNoExceptions(source,0x101000,suffix_start+127*4096,4096,plan,8*4096);
        budget=-1;
        if(status==PrepareStatus::NoMemory) {
          ++caught;if(!plan.pieces.empty()||!plan.merged.combined.live.empty()||!plan.cleanup_merged.combined.live.empty())++failed;
        } else if(status==PrepareStatus::Ok) {
          success=true;
          if(plan.pieces.size()!=3 || plan.pieces.back().private_bytes.size()!=128*4096 ||
             plan.combined_pages<=plan.first_capacity || plan.cleanup_merged.combined.live.size()!=plan.combined_pages ||
             plan.pieces.front().partitions[0].mapped().kind!=Kind::kHole ||
             plan.pieces.front().kept[0].kind!=Kind::kView || plan.pins.size()!=1 ||
             plan.pins[0].source_pins!=1 || plan.pins[0].retained_views!=1 ||
             plan.constructor_first.live.size()!=8 || plan.constructor_tail.empty() ||
             !plan.constructor_failures[0])++failed;
        } else ++failed;
      }
      if(live!=before || source_owner.refs!=2 || injected!=(status==PrepareStatus::NoMemory?1:0))++failed;
      std::printf("PUNCH_REQUEST_ALLOCATION point=%ld calls=%ld injected=%ld status=%d released=%d\n",
        point,calls,injected,static_cast<int>(status),live==before);
      if(success)break;
    }
    if(!success||!caught)++failed;
  }
  std::printf("PREPARE_RESULT failures=%d success=%d live=%ld failed=%d\n", failures,successes,live,failed);
  return failed ? 1 : 0;
}
