// CI-only pinned helper/allocator qualification; build with same exception
// runtime/ABI and source-local helper options. No synthetic NO_MEMORY return.
#include "../../compat/posix/src/mman_registry_internal.h"
#include <cstdlib>
#include <cstdio>
#include <new>
using namespace gesher_mman_internal;
namespace { long budget = -1; long live = 0; long calls = 0; }
void* operator new(std::size_t size) {
  ++calls;
  if (budget == 0) throw std::bad_alloc();
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
    calls = 0;
    {
      PreparedAlias out;
      status = PrepareAlias({0, 0, 4096, 4096, PAGE_READWRITE}, out);
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
    if (!released) ++failed;
    std::printf("PREPARE_ALLOCATION point=%ld calls=%ld status=%d empty=%d released=%d\n",
                point, calls, static_cast<int>(status), empty, released);
    if (successes) break;
  }
  budget = -1;
  PreparedAlias out;
  if (PrepareAlias({0, 0, 0, 4096, PAGE_READWRITE}, out) != PrepareStatus::InvalidSize ||
      !out.node.empty() || out.owner || failures < 3 || successes != 1) ++failed;
  std::printf("PREPARE_RESULT failures=%d success=%d live=%ld failed=%d\n", failures,successes,live,failed);
  return failed ? 1 : 0;
}
