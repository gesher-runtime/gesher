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
      status = PrepareAliasFromNoExceptions({0, 0, 4096, 4096, PAGE_READWRITE}, out);
      empty = out.node.empty() && !out.owner;
      budget = -1;
      if (status == PrepareStatus::NoMemory) { ++failures; if (!empty) ++failed; }
      else if (status == PrepareStatus::Ok) {
        ++successes;
        if (out.node.empty() || !out.owner || out.owner->state != SectionState::Borrowed ||
            out.owner->handle != nullptr || out.node.mapped().live_pages != 1) ++failed;
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
      !out.node.empty() || out.owner || failures < 3 || successes != 1) ++failed;
  {
    SectionOwner owner; owner.state=SectionState::Live; owner.refs=3;
    Region source; source.kind=Kind::kView; source.shared=true; source.owner=&owner;
    source.group=0x10000; source.size=5*4096; source.view_protection=PAGE_READWRITE;
    source.live.assign(5,true); source.live_pages=5;
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
  std::printf("PREPARE_RESULT failures=%d success=%d live=%ld failed=%d\n", failures,successes,live,failed);
  return failed ? 1 : 0;
}
