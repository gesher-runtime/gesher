// gesher-base: mmap family on Win32.
//
// Windows frees memory per allocation (VirtualAlloc) or per view
// (MapViewOfFile), while mmap callers unmap, replace and re-protect
// arbitrary page ranges. Since Windows 10 1803, placeholders let a range of
// reserved address space be split at any page and each piece replaced by an
// allocation or a file view (VirtualAlloc2 / MapViewOfFile3), which is what
// this layer builds on (docs/decisions/0036):
//
//   - Every mapping starts as a placeholder reservation (its "group"), which
//     is then filled: anonymous memory, or a view of the file. The layer keeps
//     the pieces of each group in `State::regions`: allocations, views and
//     holes (placeholders that nothing fills).
//   - MAP_FIXED over pages of a group (ART reserves address space with an
//     anonymous PROT_NONE mapping and maps an oat file's segments into it)
//     turns them back into a placeholder - an allocation gives its pages up,
//     a view is unmapped and what stays of it mapped again - and fills that
//     with the new mapping. Oat files' code is mapped, not copied: an
//     executable view of the file (the handle reopened with GENERIC_EXECUTE),
//     shared between processes until a page is written.
//   - munmap gives an allocation's pages back to the placeholder; a view's
//     pages become inaccessible until none of the view is left. When nothing
//     of a group is mapped any more, its placeholders are freed.
//   - A view covers whole pages of the file: the part of a private mapping
//     from the last partial page of the file on is anonymous memory with the
//     file's bytes copied in (it reads the same). A shared mapping with a
//     partial last page is a plain view (MapViewOfFileEx) of the file up to
//     its end, as before placeholders, and cannot be replaced by MAP_FIXED.
//
// Without placeholders (older Windows, or GESHER_MMAP_NO_PLACEHOLDERS=1),
// mappings are plain allocations and views, tracked with a per-page liveness
// bitmap: unmapping part of an anonymous region decommits those pages, part
// of a view makes them inaccessible, and when no live page remains the
// allocation or view is released. A private file mapping with MAP_FIXED in
// one of the layer's anonymous regions then gets the file's contents copied
// in; MAP_FIXED over a view, or over memory the layer does not own, fails
// with EINVAL.
//
// mremap moves anonymous mappings by copying (see mremap below).

#include <sys/mman.h>
#include <sys/resource.h>

#include <errno.h>
#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "binder_internal.h"
#include "internal.h"

// Dedicated failure object only; normal production route is fixed Win32.
#ifdef GESHER_FILE_MREMAP_FAILURE_TEST
#include "../../../tests/file_mremap/alias_failure_adapter.h"
#else
#define FileAliasDuplicateHandle DuplicateHandle
#define FileAliasMapViewOfFile MapViewOfFile
#define FileAliasCloseHandle CloseHandle
#endif

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

namespace {

// Loader copy bookkeeping needs to distinguish a failed plain MAP_FIXED that
// already decommitted its old pages from a failure before replacement. The
// result is consumed immediately after mmap on the same thread.
thread_local bool g_mmap_replaced_private_pages = false;

// The placeholder functions, looked up at run time (kernelbase.dll, Windows
// 10 1803 and later).
struct PlaceholderApi {
  using Alloc2 = void*(WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG, void*, ULONG);
  using Map3 = void*(WINAPI*)(HANDLE, HANDLE, void*, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
  using Unmap2 = BOOL(WINAPI*)(HANDLE, void*, ULONG);
  Alloc2 alloc2 = nullptr;
  Map3 map3 = nullptr;
  Unmap2 unmap2 = nullptr;

  PlaceholderApi() {
    if (getenv("GESHER_MMAP_NO_PLACEHOLDERS") != nullptr) return;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase == nullptr) return;
    alloc2 = reinterpret_cast<Alloc2>(
        reinterpret_cast<void*>(GetProcAddress(kernelbase, "VirtualAlloc2")));
    map3 = reinterpret_cast<Map3>(
        reinterpret_cast<void*>(GetProcAddress(kernelbase, "MapViewOfFile3")));
    unmap2 = reinterpret_cast<Unmap2>(
        reinterpret_cast<void*>(GetProcAddress(kernelbase, "UnmapViewOfFile2")));
    if (map3 == nullptr || unmap2 == nullptr) alloc2 = nullptr;
  }
  bool available() const { return alloc2 != nullptr; }
};

const PlaceholderApi& Placeholders() {
  static const PlaceholderApi* api = new PlaceholderApi();
  return *api;
}

enum class Kind : uint8_t {
  kAnon,  // an allocation
  kView,  // a view of a file
  kHole,  // a placeholder (group regions only)
};

// Windows' allocation type is not the mapping's Linux origin: private file
// tails and MAP_FIXED file fallbacks also use VirtualAlloc. Plain allocations
// may contain several origins after fixed replacement, so keep one byte per
// logical page (1/4096 of the mapping on supported Windows systems).
enum class Origin : uint8_t {
  kUnknown,
  kPrivateAnonymous,
  kSharedAnonymous,
  kPrivateFile,
  kSharedFile,
  kDevice,
};

struct Region {
  size_t size = 0;            // bytes, multiple of the page size
  Kind kind = Kind::kAnon;
  // The placeholder reservation (its start) the region is a piece of; 0 for
  // a plain allocation or view, made without placeholders.
  uintptr_t group = 0;
  void* view = nullptr;       // views: what MapViewOfFile* returned
  HANDLE mapping = nullptr;   // views: the mapping object (owned)
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
  std::vector<Origin> origins;  // logical origin; independent of Kind/MEM_PRIVATE
  size_t live_pages = 0;
};

struct State {
  std::mutex lock;
  std::map<uintptr_t, Region> regions;  // keyed by region start
  std::map<uintptr_t, size_t> groups;   // placeholder reservations: start, size
  // Only pages explicitly locked by the guest. Win32's working set is not
  // the guest's lock ledger, and a repeated lock must not spend its quota twice.
  std::set<uintptr_t> locked_pages;
  size_t page_size;
  size_t granularity;

  State() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page_size = si.dwPageSize;
    granularity = si.dwAllocationGranularity;
  }
};

// Never destroyed: mappings outlive static destructors.
State& state() {
  static State* s = new State();
  return *s;
}

size_t RoundUp(size_t x, size_t n) { return (x + n - 1) / n * n; }

// Validate before rounding or forming a range end. Zero length still requires
// a page-aligned address for madvise, as on Linux.
bool AdviceRange(uintptr_t begin, size_t length, size_t page, size_t* rounded) {
  if (begin % page != 0 || length > SIZE_MAX - (page - 1)) {
    errno = EINVAL;
    return false;
  }
  *rounded = RoundUp(length, page);
  if (*rounded > UINTPTR_MAX - begin) {
    errno = EINVAL;
    return false;
  }
  return true;
}

// GESHER_TRACE_MMAP=1 logs every call to stderr.
bool Tracing() {
  static const bool on = getenv("GESHER_TRACE_MMAP") != nullptr;
  return on;
}
#define TRACE(...) (Tracing() ? (void)fprintf(stderr, "gesher-mmap: " __VA_ARGS__) : (void)0)

DWORD PageProtection(int prot) {
  switch (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case PROT_NONE: return PAGE_NOACCESS;
    case PROT_READ: return PAGE_READONLY;
    case PROT_WRITE:
    case PROT_READ | PROT_WRITE: return PAGE_READWRITE;
    case PROT_EXEC:
    case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
  }
}

// The allocation type for anonymous memory with `prot`. Linux backs anonymous
// memory lazily, so ART reserves address space with large PROT_NONE mappings
// (the CMC from-space, LinearAlloc shadow maps, aligned-map slack) that cost
// nothing there. Committing them on Windows charged gigabytes of commit per
// process, so PROT_NONE anonymous pages are reserved only: nothing can touch
// them, and mprotect commits them when they become accessible.
DWORD AnonymousCommit(int prot) {
  return (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == PROT_NONE ? MEM_RESERVE
                                                                  : MEM_RESERVE | MEM_COMMIT;
}

// What a private (copy-on-write) view's pages take for `protection`.
DWORD CopyOnWrite(DWORD protection) {
  if (protection == PAGE_READWRITE) return PAGE_WRITECOPY;
  if (protection == PAGE_EXECUTE_READWRITE) return PAGE_EXECUTE_WRITECOPY;
  return protection;
}

// Region containing `addr`, or end().
std::map<uintptr_t, Region>::iterator FindRegion(State& s, uintptr_t addr) {
  auto it = s.regions.upper_bound(addr);
  if (it == s.regions.begin()) return s.regions.end();
  --it;
  return addr < it->first + it->second.size ? it : s.regions.end();
}

std::map<uintptr_t, Region>::iterator AddRegion(State& s, uintptr_t start, size_t size,
                                                Region region, Origin origin = Origin::kUnknown) {
  region.size = size;
  const bool live = region.kind != Kind::kHole;
  region.live.assign(size / s.page_size, live);
  region.origins.assign(size / s.page_size, origin);
  region.live_pages = live ? region.live.size() : 0;
  return s.regions.emplace(start, std::move(region)).first;
}

void ForgetLocks(State& s, uintptr_t begin, uintptr_t end) {
  s.locked_pages.erase(s.locked_pages.lower_bound(begin), s.locked_pages.lower_bound(end));
}

// Caller holds State::lock. Registry operations never call public mprotect/
// mlock; Win32 protection/locking functions do not reenter this registry.
// Invalidate conservatively even if the requested operation later fails.
void InvalidateSharedAliases(State& s, uintptr_t begin, size_t length) {
  const uintptr_t end = begin + length;
  auto it = FindRegion(s, begin);
  if (it == s.regions.end()) it = s.regions.lower_bound(begin);
  for (; it != s.regions.end() && it->first < end; ++it) {
    if (!it->second.shared_alias) continue;
    const uintptr_t at = it->first;
    const size_t size = it->second.size;
    if ((at <= begin && begin - at < size) || (begin < at && at - begin < length))
      it->second.shared_alias = false;
  }
}

// Slice A never retires source. It duplicates the retained section, not a
// descriptor integer or pathname. All allocation occurs before a view exists.
void* SharedAlias(State& s, uintptr_t begin, size_t old_size, size_t length, int flags) {
  auto source = FindRegion(s, begin);
  if (source == s.regions.end() || source->first != begin) { errno = EINVAL; return MAP_FAILED; }
  const Region& from = source->second;
  if (!from.shared_alias || !from.shared || from.kind != Kind::kView ||
      from.live_pages != from.live.size() || length > from.size ||
      (flags != 0 && flags != MREMAP_MAYMOVE)) { errno = EINVAL; return MAP_FAILED; }
  const DWORD protection = from.view_protection;
  for (size_t at = 0; at < from.size; at += s.page_size) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<void*>(begin + at), &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || info.Type != MEM_MAPPED || info.Protect != protection) {
      errno = EINVAL; return MAP_FAILED;
    }
  }
  if (old_size != 0) {
    if (old_size == length && old_size == from.size) return reinterpret_cast<void*>(begin);
    errno = EINVAL; return MAP_FAILED;  // resize is a visible outstanding family
  }
  if (flags != MREMAP_MAYMOVE) { errno = EINVAL; return MAP_FAILED; }
  const size_t padding = static_cast<size_t>(from.offset % s.granularity);
  const uint64_t section_offset = from.offset - padding;
  if (length > SIZE_MAX - padding) {
    errno = EINVAL; return MAP_FAILED;
  }
  Region alias;
  alias.kind = Kind::kView;
  alias.shared = true;
  alias.shared_alias = true;
  alias.offset = from.offset;
  alias.view_protection = protection;
  alias.view_padding = padding;
  alias.physical_extent = length + alias.view_padding;
  alias.size = length;
  alias.live.assign(length / s.page_size, true);
  alias.origins.assign(length / s.page_size, Origin::kSharedFile);
  alias.live_pages = alias.live.size();
  // A C++17 node handle makes final address publication allocation-free.
  std::map<uintptr_t, Region> prepared;
  prepared.emplace(0, std::move(alias));
  auto node = prepared.extract(prepared.begin());
  HANDLE mapping = nullptr;
  if (!FileAliasDuplicateHandle(GetCurrentProcess(), from.mapping, GetCurrentProcess(), &mapping,
                       0, FALSE, DUPLICATE_SAME_ACCESS)) { errno = ENOMEM; return MAP_FAILED; }
  const DWORD access = protection == PAGE_READWRITE ? FILE_MAP_WRITE : FILE_MAP_READ;
  void* view = FileAliasMapViewOfFile(mapping, access, static_cast<DWORD>(section_offset >> 32),
                             static_cast<DWORD>(section_offset), node.mapped().physical_extent);
  if (view == nullptr) {
    const DWORD error = GetLastError();
    FileAliasCloseHandle(mapping);  // exact successfully duplicated handle, never borrowed
    errno = error == ERROR_ACCESS_DENIED ? EACCES : ENOMEM;
    return MAP_FAILED;
  }
  const uintptr_t result = reinterpret_cast<uintptr_t>(view) + node.mapped().view_padding;
  node.key() = result;
  node.mapped().mapping = mapping;
  node.mapped().view = view;
  // Fresh nonfixed OS view cannot overlap an existing live registry range.
  s.regions.insert(std::move(node));
  return reinterpret_cast<void*>(result);
}

// --- placeholder groups -----------------------------------------------------

// Splits the bookkeeping entry `it` at `at` (inside it); returns the upper
// part. Windows' own split is the caller's.
std::map<uintptr_t, Region>::iterator SplitEntry(State& s,
                                                 std::map<uintptr_t, Region>::iterator it,
                                                 uintptr_t at) {
  Region& lower = it->second;
  const size_t lower_pages = (at - it->first) / s.page_size;
  Region upper = lower;
  lower.shared_alias = upper.shared_alias = false;
  upper.size = lower.size - (at - it->first);
  upper.live.assign(lower.live.begin() + lower_pages, lower.live.end());
  upper.origins.assign(lower.origins.begin() + lower_pages, lower.origins.end());
  upper.live_pages = static_cast<size_t>(std::count(upper.live.begin(), upper.live.end(), true));
  upper.offset = lower.offset + (at - it->first);
  lower.size = at - it->first;
  lower.live.resize(lower_pages);
  lower.origins.resize(lower_pages);
  lower.live_pages -= upper.live_pages;
  return s.regions.emplace(at, std::move(upper)).first;
}

// A new placeholder reservation of `len` bytes at `at` (exactly) or, with
// nullptr, anywhere: one hole. Returns its start, or 0.
uintptr_t Reserve(State& s, void* at, size_t len) {
  void* p = Placeholders().alloc2(GetCurrentProcess(), at, len,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
  if (p == nullptr) return 0;
  const uintptr_t start = reinterpret_cast<uintptr_t>(p);
  Region hole;
  hole.kind = Kind::kHole;
  hole.group = start;
  AddRegion(s, start, len, std::move(hole));
  s.groups[start] = len;
  return start;
}

// A reservation for a mapping with a hint (not MAP_FIXED): at the hint, else
// at the next allocation granularity boundary after it (reservations start at
// one), else anywhere. The second try keeps ART's search for free address
// space in the low 4 GiB, which steps through pages, from ending up anywhere
// at each page that is not on a boundary.
uintptr_t ReserveNear(State& s, void* hint, size_t len) {
  uintptr_t at = 0;
  if (hint != nullptr) {
    at = Reserve(s, hint, len);
    const uintptr_t next = RoundUp(reinterpret_cast<uintptr_t>(hint), s.granularity);
    if (at == 0 && next != reinterpret_cast<uintptr_t>(hint)) {
      at = Reserve(s, reinterpret_cast<void*>(next), len);
    }
  }
  return at != 0 ? at : Reserve(s, nullptr, len);
}

// Frees a group's placeholders once nothing of it is mapped.
void ReleaseGroupIfEmpty(State& s, uintptr_t group) {
  auto g = s.groups.find(group);
  if (g == s.groups.end()) return;
  const uintptr_t end = group + g->second;
  for (auto it = s.regions.lower_bound(group); it != s.regions.end() && it->first < end; ++it) {
    if (it->second.kind != Kind::kHole) return;
  }
  for (auto it = s.regions.lower_bound(group); it != s.regions.end() && it->first < end;) {
    VirtualFree(reinterpret_cast<void*>(it->first), 0, MEM_RELEASE);
    it = s.regions.erase(it);
  }
  s.groups.erase(g);
}

// The group whose regions cover [begin, end) without a gap, or 0.
uintptr_t CoveringGroup(State& s, uintptr_t begin, uintptr_t end) {
  auto it = FindRegion(s, begin);
  if (it == s.regions.end() || it->second.group == 0) return 0;
  const uintptr_t group = it->second.group;
  for (uintptr_t at = it->first + it->second.size; at < end; at += it->second.size) {
    ++it;
    if (it == s.regions.end() || it->first != at || it->second.group != group) return 0;
  }
  return group;
}

bool RangeIsOurs(State& s, uintptr_t begin, uintptr_t end) {
  auto it = FindRegion(s, begin);
  if (it == s.regions.end()) it = s.regions.lower_bound(begin);
  return it != s.regions.end() && it->first < end;
}

// A placeholder retained after munmap is address-space bookkeeping, not a
// live Linux mapping. MAP_FIXED_NOREPLACE may reuse an entirely unmapped range
// inside a group, but must reject the complete request before touching even
// one page when any part is still mapped (including PROT_NONE reservations).
bool RangeIsHole(State& s, uintptr_t begin, uintptr_t end) {
  auto it = FindRegion(s, begin);
  for (uintptr_t at = begin; at < end; ++it) {
    if (it == s.regions.end() || it->first > at || it->second.group == 0 ||
        it->second.kind != Kind::kHole) {
      return false;
    }
    at = it->first + it->second.size;
  }
  return true;
}

// Maps a view of `mapping` into the hole exactly at [start, start + size).
bool MapViewInto(HANDLE mapping, uintptr_t start, size_t size, uint64_t offset, ULONG protection) {
  return Placeholders().map3(mapping, GetCurrentProcess(), reinterpret_cast<void*>(start), offset,
                             size, MEM_REPLACE_PLACEHOLDER, protection, nullptr, 0) != nullptr;
}

// One page of a view, as it was before the view was taken down.
struct SavedPage {
  DWORD protection;
  bool live;
  std::vector<uint8_t> bytes;  // private views' live pages
};

// [x, y) inside the view region `it` becomes a hole; the rest of the view is
// mapped again, with the protections and private changes it had.
std::map<uintptr_t, Region>::iterator PunchView(State& s, std::map<uintptr_t, Region>::iterator it,
                                                uintptr_t x, uintptr_t y) {
  const uintptr_t start = it->first;
  const Region view = it->second;
  const uintptr_t end = start + view.size;
  const size_t page = s.page_size;
  // What stays: [start, x) and [y, end).
  std::vector<SavedPage> saved(view.size / page);
  for (uintptr_t p = start; p < end; p += page) {
    if (p >= x && p < y) continue;
    SavedPage& sp = saved[(p - start) / page];
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(reinterpret_cast<void*>(p), &info, sizeof(info));
    sp.protection = info.Protect & ~PAGE_GUARD;
    sp.live = view.live[(p - start) / page];
    if (!view.shared && sp.live) {
      DWORD old;
      const bool readable = sp.protection != PAGE_NOACCESS;
      if (!readable) VirtualProtect(reinterpret_cast<void*>(p), page, PAGE_READONLY, &old);
      sp.bytes.assign(reinterpret_cast<uint8_t*>(p), reinterpret_cast<uint8_t*>(p) + page);
      if (!readable) VirtualProtect(reinterpret_cast<void*>(p), page, PAGE_NOACCESS, &old);
    }
  }
  if (!Placeholders().unmap2(GetCurrentProcess(), view.view, MEM_PRESERVE_PLACEHOLDER)) {
    gesher::FailWithLastError();
    return s.regions.end();
  }
  ForgetLocks(s, x, y);
  it->second.kind = Kind::kHole;
  it->second.view = nullptr;
  it->second.mapping = nullptr;
  std::fill(it->second.live.begin(), it->second.live.end(), false);
  it->second.live_pages = 0;
  // Split the placeholder at x and y, and map the pieces that stay.
  auto remap = [&](uintptr_t from, uintptr_t to) {
    if (from >= to) return;
    auto piece = FindRegion(s, from);
    if (piece->first != from) piece = SplitEntry(s, piece, from);
    if (piece->first + piece->second.size != to) SplitEntry(s, piece, to);
    if (to - from != view.size) {
      VirtualFree(reinterpret_cast<void*>(from), to - from, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    }
    HANDLE mapping = nullptr;
    DuplicateHandle(GetCurrentProcess(), view.mapping, GetCurrentProcess(), &mapping, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    const uint64_t offset = view.offset + (from - start);
    if (mapping == nullptr || !MapViewInto(mapping, from, to - from, offset, view.view_protection)) {
      // Cannot happen short of running out of resources: the pages read as
      // a placeholder (they fault), which is better than wrong contents.
      TRACE("remapping [%p, %p) of a view failed (%lu)\n", reinterpret_cast<void*>(from),
            reinterpret_cast<void*>(to), GetLastError());
      if (mapping != nullptr) CloseHandle(mapping);
      return;
    }
    Region& r = piece->second;
    r.kind = Kind::kView;
    r.view = reinterpret_cast<void*>(from);
    r.mapping = mapping;
    r.offset = offset;
    r.view_protection = view.view_protection;
    r.shared = view.shared;
    r.live_pages = 0;
    for (uintptr_t p = from; p < to; p += page) {
      const SavedPage& sp = saved[(p - start) / page];
      DWORD old;
      if (!sp.bytes.empty() && memcmp(reinterpret_cast<void*>(p), sp.bytes.data(), page) != 0) {
        // A page the process had written (a private copy): written again.
        VirtualProtect(reinterpret_cast<void*>(p), page, PAGE_WRITECOPY, &old);
        memcpy(reinterpret_cast<void*>(p), sp.bytes.data(), page);
      }
      VirtualProtect(reinterpret_cast<void*>(p), page,
                     view.shared ? sp.protection : CopyOnWrite(sp.protection), &old);
      r.live[(p - from) / page] = sp.live;
      if (sp.live) ++r.live_pages;
    }
  };
  remap(start, std::max(start, x));
  remap(std::min(end, y), end);
  CloseHandle(view.mapping);
  // The punched part: a hole entry of its own.
  auto hole = FindRegion(s, std::max(start, x));
  if (hole->first != std::max(start, x)) hole = SplitEntry(s, hole, std::max(start, x));
  if (hole->first + hole->second.size > std::min(end, y)) SplitEntry(s, hole, std::min(end, y));
  return hole;
}

// [begin, end), covered by regions of one group, becomes one placeholder (a
// single hole entry), which is returned.
std::map<uintptr_t, Region>::iterator Punch(State& s, uintptr_t begin, uintptr_t end) {
  // A partial replacement of a view takes down the whole Win32 view. Until
  // that path can reliably reconstruct its locks, keep retained pages pinned.
  for (auto it = FindRegion(s, begin); it != s.regions.end() && it->first < end; ++it) {
    if (it->second.kind != Kind::kView) continue;
    const uintptr_t start = it->first;
    const uintptr_t stop = start + it->second.size;
    auto locked = s.locked_pages.lower_bound(start);
    if (start < begin && locked != s.locked_pages.end() && *locked < begin) {
      errno = EOPNOTSUPP;
      return s.regions.end();
    }
    locked = s.locked_pages.lower_bound(end);
    if (end < stop && locked != s.locked_pages.end() && *locked < stop) {
      errno = EOPNOTSUPP;
      return s.regions.end();
    }
  }
  for (uintptr_t at = begin; at < end;) {
    auto it = FindRegion(s, at);
    const uintptr_t region_end = it->first + it->second.size;
    const uintptr_t x = at;
    const uintptr_t y = std::min(end, region_end);
    switch (it->second.kind) {
      case Kind::kHole:
        if (it->first != x || region_end != y) {
          VirtualFree(reinterpret_cast<void*>(x), y - x, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
          if (it->first != x) it = SplitEntry(s, it, x);
          if (it->first + it->second.size != y) SplitEntry(s, it, y);
        }
        break;
      case Kind::kAnon: {
        if (!VirtualFree(reinterpret_cast<void*>(x), y - x,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
          gesher::FailWithLastError();
          return s.regions.end();
        }
        ForgetLocks(s, x, y);
        if (it->first != x) it = SplitEntry(s, it, x);
        if (it->first + it->second.size != y) SplitEntry(s, it, y);
        Region& r = it->second;
        r.kind = Kind::kHole;
        std::fill(r.live.begin(), r.live.end(), false);
        r.live_pages = 0;
        break;
      }
      case Kind::kView:
        if (it->first == x && region_end == y) {
          if (!Placeholders().unmap2(GetCurrentProcess(), it->second.view,
                                      MEM_PRESERVE_PLACEHOLDER)) {
            gesher::FailWithLastError();
            return s.regions.end();
          }
          ForgetLocks(s, x, y);
          CloseHandle(it->second.mapping);
          Region& r = it->second;
          r.kind = Kind::kHole;
          r.view = nullptr;
          r.mapping = nullptr;
          std::fill(r.live.begin(), r.live.end(), false);
          r.live_pages = 0;
        } else if (PunchView(s, it, x, y) == s.regions.end()) {
          return s.regions.end();
        }
        break;
    }
    at = y;
  }
  // One placeholder for the whole range.
  auto first = FindRegion(s, begin);
  if (first->second.size != end - begin) {
    VirtualFree(reinterpret_cast<void*>(begin), end - begin,
                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    for (auto next = std::next(first); next != s.regions.end() && next->first < end;) {
      next = s.regions.erase(next);
    }
    first->second.size = end - begin;
    first->second.live.assign((end - begin) / s.page_size, false);
    first->second.origins.assign((end - begin) / s.page_size, Origin::kUnknown);
    first->second.live_pages = 0;
  }
  return first;
}

// Fills the hole `it` with anonymous zero pages. PROT_NONE pages are only
// reserved (see AnonymousCommit).
bool FillAnonymous(std::map<uintptr_t, Region>::iterator it, int prot, Origin origin) {
  if (Placeholders().alloc2(GetCurrentProcess(), reinterpret_cast<void*>(it->first),
                            it->second.size, AnonymousCommit(prot) | MEM_REPLACE_PLACEHOLDER,
                            PageProtection(prot), nullptr, 0) == nullptr) {
    return false;
  }
  Region& r = it->second;
  r.kind = Kind::kAnon;
  std::fill(r.live.begin(), r.live.end(), true);
  std::fill(r.origins.begin(), r.origins.end(), origin);
  r.live_pages = r.live.size();
  return true;
}

// Anonymous memory in a group: at `start` (MAP_FIXED over the group's pages,
// or into free address space), or anywhere.
void* GroupAnonymous(State& s, void* start, size_t len, int prot, int flags) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  std::map<uintptr_t, Region>::iterator hole;
  if (fixed && CoveringGroup(s, addr, addr + len) != 0) {
    if ((flags & MAP_FIXED_NOREPLACE) != 0 && !RangeIsHole(s, addr, addr + len)) {
      errno = EEXIST;
      return MAP_FAILED;
    }
    hole = Punch(s, addr, addr + len);
    if (hole == s.regions.end()) return MAP_FAILED;
  } else {
    if (fixed && RangeIsOurs(s, addr, addr + len)) {
      errno = (flags & MAP_FIXED_NOREPLACE) != 0 ? EEXIST : EINVAL;
      return MAP_FAILED;
    }
    uintptr_t at = fixed ? Reserve(s, start, len) : ReserveNear(s, start, len);
    if (at == 0 && fixed) {
      errno = (flags & MAP_FIXED_NOREPLACE) != 0 ? EEXIST : EINVAL;
      return MAP_FAILED;
    }
    if (at == 0) {
      errno = ENOMEM;
      return MAP_FAILED;
    }
    hole = s.regions.find(at);
  }
  const uintptr_t group = hole->second.group;
  const uintptr_t result = hole->first;
  const Origin origin = (flags & MAP_SHARED) != 0 ? Origin::kSharedAnonymous
                                                : Origin::kPrivateAnonymous;
  if (!FillAnonymous(hole, prot, origin)) {
    ReleaseGroupIfEmpty(s, group);
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return reinterpret_cast<void*>(result);
}

// The file's mapped bytes from `offset` into [dest, dest + len), including
// cached bytes beyond EOF in its final partial page. They are visible to a
// later mmap without extending st_size or becoming readable by pread(EOF).
// A native view supplies both file identity and cache lifetime; no fd-number
// cache or write past the logical EOF is involved.
bool CopyFromFile(State& s, HANDLE file, uint64_t file_size, uint64_t offset, uint8_t* dest,
                  size_t len) {
  const uint64_t mapped_size = (file_size + s.page_size - 1) / s.page_size * s.page_size;
  if (offset >= mapped_size) return true;
  const size_t available = static_cast<size_t>(std::min<uint64_t>(len, mapped_size - offset));
  const size_t padding = static_cast<size_t>(offset % s.granularity);
  const uint64_t view_offset = offset - padding;
  // Explicit view lengths cannot exceed the logical section size. Zero maps
  // through its end, including the physical last page, without growing it.
  const size_t view_len = offset < file_size && available <= file_size - offset
                             ? available + padding : 0;
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  void* view = mapping == nullptr ? nullptr
      : MapViewOfFile(mapping, FILE_MAP_READ, static_cast<DWORD>(view_offset >> 32),
                      static_cast<DWORD>(view_offset & 0xffffffffu), view_len);
  if (view == nullptr) {
    if (mapping != nullptr) CloseHandle(mapping);
    return false;
  }
  memcpy(dest, static_cast<uint8_t*>(view) + padding, available);
  UnmapViewOfFile(view);
  CloseHandle(mapping);
  return true;
}

// A file mapping in a group. Returns MAP_FAILED with errno = 0 when it has to
// be a plain view instead (a shared mapping that ends in a partial page).
void* GroupFile(State& s, void* start, size_t len, int prot, int flags, int fd, off_t offset) {
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  LARGE_INTEGER file_length;
  if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &file_length)) {
    errno = EBADF;
    return MAP_FAILED;
  }
  const uint64_t file_size = static_cast<uint64_t>(file_length.QuadPart);
  const uint64_t begin = static_cast<uint64_t>(offset);
  const bool shared = (flags & MAP_SHARED) != 0;
  const bool exec = (prot & PROT_EXEC) != 0;
  const bool write = (prot & PROT_WRITE) != 0;
  // Whole pages of the file go into the view; the rest is anonymous.
  const size_t in_file = begin >= file_size ? 0
      : static_cast<size_t>(std::min<uint64_t>(len, (file_size - begin) / s.page_size * s.page_size));
  if (shared && in_file < len) {
    errno = 0;
    return MAP_FAILED;
  }

  // The mapping object, executable views from a handle that may execute.
  // (A private mapping of a file that cannot be reopened so is copied.) A
  // private mapping without PROT_EXEC gets an executable section too when the
  // file allows it, so mprotect can add PROT_EXEC later, as on Linux (packers
  // and protectors mprotect a whole library RWX); else a plain one.
  size_t in_view = in_file;
  HANDLE mapping = nullptr;
  ULONG view_protection = 0;
  HANDLE reopened = nullptr;
  bool exec_section = false;
  if (in_view != 0 && (exec || !shared)) {
    reopened = ReOpenFile(file, GENERIC_READ | GENERIC_EXECUTE | (shared && write ? GENERIC_WRITE : 0),
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    exec_section = reopened != INVALID_HANDLE_VALUE;
    if (reopened == INVALID_HANDLE_VALUE && !exec) {
      reopened = nullptr;  // a plain copy-on-write section
    } else if (reopened == INVALID_HANDLE_VALUE) {
      reopened = nullptr;
      if (shared) {
        errno = EACCES;
        return MAP_FAILED;
      }
      TRACE("no execute access to fd %d (%lu): its pages are copied\n", fd,
            GetLastError());
      in_view = 0;
    }
  }
  if (in_view != 0) {
    HANDLE source = reopened != nullptr ? reopened : file;
    DWORD section;
    if (shared) {
      section = write ? (exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE)
                      : (exec ? PAGE_EXECUTE_READ : PAGE_READONLY);
      view_protection = section;
    } else {
      section = exec_section ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
      view_protection = section;
    }
    mapping = CreateFileMappingW(source, nullptr, section, 0, 0, nullptr);
    if (reopened != nullptr) CloseHandle(reopened);
    if (mapping == nullptr) {
      errno = EACCES;
      return MAP_FAILED;
    }
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  std::map<uintptr_t, Region>::iterator hole;
  if (fixed && CoveringGroup(s, addr, addr + len) != 0) {
    if ((flags & MAP_FIXED_NOREPLACE) != 0 && !RangeIsHole(s, addr, addr + len)) {
      if (mapping != nullptr) CloseHandle(mapping);
      errno = EEXIST;
      return MAP_FAILED;
    }
    hole = Punch(s, addr, addr + len);
    if (hole == s.regions.end()) {
      if (mapping != nullptr) CloseHandle(mapping);
      return MAP_FAILED;
    }
  } else {
    uintptr_t at = 0;
    if (!(fixed && RangeIsOurs(s, addr, addr + len))) {
      at = fixed ? Reserve(s, start, len) : ReserveNear(s, start, len);
    }
    if (at == 0) {
      if (mapping != nullptr) CloseHandle(mapping);
      errno = fixed ? ((flags & MAP_FIXED_NOREPLACE) != 0 ? EEXIST : EINVAL) : ENOMEM;
      return MAP_FAILED;
    }
    hole = s.regions.find(at);
  }
  const uintptr_t result = hole->first;
  const uintptr_t group = hole->second.group;
  auto fail = [&](int error) -> void* {
    // Undo: the pages go back to a placeholder.
    Punch(s, result, result + len);
    ReleaseGroupIfEmpty(s, group);
    errno = error;
    return MAP_FAILED;
  };

  // [result, result + in_view): the view.
  if (in_view != 0) {
    if (in_view != len) {
      VirtualFree(reinterpret_cast<void*>(result), in_view, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
      SplitEntry(s, hole, result + in_view);
    }
    // A private view that is not writable is mapped so: mapped copy-on-write
    // it is charged to commit in full until the VirtualProtect below gives
    // that back (a native library's code, an oat file). An executable
    // section's view is mapped executable (read-execute, or execute-copy-on-
    // write if writable): a view first mapped without execute can never get
    // it, and the VirtualProtect below lowers it to `prot`.
    ULONG initial = view_protection;
    if (!shared && !write && (prot & (PROT_READ | PROT_EXEC)) != 0) {
      initial = exec_section ? PAGE_EXECUTE_READ : PAGE_READONLY;
    }
    if (!MapViewInto(mapping, result, in_view, begin, initial)) {
      TRACE("MapViewOfFile3 at %p (0x%zx bytes, offset 0x%llx) failed: %lu\n",
            reinterpret_cast<void*>(result), in_view, static_cast<unsigned long long>(begin),
            GetLastError());
      CloseHandle(mapping);
      return fail(EACCES);
    }
    Region& r = hole->second;
    r.kind = Kind::kView;
    r.view = reinterpret_cast<void*>(result);
    r.mapping = mapping;
    r.offset = begin;
    r.view_protection = view_protection;
    r.shared = shared;
    r.physical_extent = in_view;
    r.view_padding = 0;
    r.shared_alias = shared && !exec && (prot == PROT_READ || prot == (PROT_READ | PROT_WRITE)) &&
                     GetFileType(file) == FILE_TYPE_DISK;
    std::fill(r.live.begin(), r.live.end(), true);
    std::fill(r.origins.begin(), r.origins.end(),
              shared ? Origin::kSharedFile : Origin::kPrivateFile);
    r.live_pages = r.live.size();
    DWORD protection = PageProtection(prot);
    if (!shared) protection = CopyOnWrite(protection);
    DWORD old;
    if (protection != initial &&
        !VirtualProtect(reinterpret_cast<void*>(result), in_view, protection, &old)) {
      return fail(EACCES);
    }
  }
  // [result + in_view, result + len): anonymous, with what the file has there.
  if (in_view < len) {
    auto tail = FindRegion(s, result + in_view);
    if (!FillAnonymous(tail, PROT_READ | PROT_WRITE, Origin::kPrivateFile)) return fail(ENOMEM);
    if (!CopyFromFile(s, file, file_size, begin + in_view,
                      reinterpret_cast<uint8_t*>(result + in_view), len - in_view)) {
      return fail(EACCES);
    }
    DWORD old;
    if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != (PROT_READ | PROT_WRITE) &&
        !VirtualProtect(reinterpret_cast<void*>(result + in_view), len - in_view,
                        PageProtection(prot), &old)) {
      return fail(EACCES);
    }
  }
  if (exec) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(result), len);
  TRACE("fd %d at %p: %s view of 0x%zx bytes, 0x%zx bytes copied\n", fd,
        reinterpret_cast<void*>(result), in_view == 0 ? "no" : exec ? "an executable" : "a",
        in_view, len - in_view);
  return reinterpret_cast<void*>(result);
}

// --- plain allocations and views (no placeholders) --------------------------

void* MapAnonymous(State& s, void* start, size_t len, int prot, int flags) {
  const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  const bool no_replace = (flags & MAP_FIXED_NOREPLACE) != 0;
  if (Placeholders().available()) {
    // Fixed mappings within a plain region (made before placeholders were
    // used, or by the binder layer) fall through to its per-page bookkeeping.
    const uintptr_t addr = reinterpret_cast<uintptr_t>(start);
    auto it = FindRegion(s, addr);
    if (it == s.regions.end() || it->second.group != 0 || !fixed) {
      return GroupAnonymous(s, start, len, prot, flags);
    }
  }
  DWORD protection = PageProtection(prot);
  uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  const Origin origin = (flags & MAP_SHARED) != 0 ? Origin::kSharedAnonymous
                                                : Origin::kPrivateAnonymous;

  // A partial munmap leaves dead pages inside the plain Windows reservation.
  // NOREPLACE may reuse those holes, but a live page (including PROT_NONE)
  // rejects the entire request before any native or bookkeeping mutation.
  if (fixed) {
    auto it = FindRegion(s, addr);
    if (it != s.regions.end() && it->second.kind == Kind::kAnon && it->second.group == 0 &&
        addr + len <= it->first + it->second.size) {
      Region& region = it->second;
      const size_t first = (addr - it->first) / s.page_size;
      const size_t last = first + len / s.page_size;
      if (no_replace && std::any_of(region.live.begin() + first, region.live.begin() + last,
                                   [](bool live) { return live; })) {
        errno = EEXIST;
        return MAP_FAILED;
      }
      // Holes were already decommitted by munmap. Retain them on recommit
      // failure; ordinary MAP_FIXED still discards the pages it replaces.
      if (!no_replace) {
        if (!VirtualFree(start, len, MEM_DECOMMIT)) {
          errno = gesher::ErrnoFromWin32(GetLastError());
          return MAP_FAILED;
        }
        ForgetLocks(s, addr, addr + len);
        g_mmap_replaced_private_pages = true;
        for (size_t i = first; i < last; ++i) {
          if (region.live[i]) --region.live_pages;
          region.live[i] = false;
        }
      }
      if ((AnonymousCommit(prot) & MEM_COMMIT) != 0 &&
          VirtualAlloc(start, len, MEM_COMMIT, protection) == nullptr) {
        errno = ENOMEM;
        return MAP_FAILED;
      }
      for (size_t i = first; i < last; ++i) {
        region.origins[i] = origin;
        if (!region.live[i]) {
          region.live[i] = true;
          ++region.live_pages;
        }
      }
      return start;
    }
  }

  void* result = nullptr;
  if (start != nullptr) {
    result = VirtualAlloc(start, len, AnonymousCommit(prot), protection);
    if (result == nullptr && fixed) {
      errno = (flags & MAP_FIXED_NOREPLACE) != 0 ? EEXIST : EINVAL;
      return MAP_FAILED;
    }
  }
  if (result == nullptr) {
    result = VirtualAlloc(nullptr, len, AnonymousCommit(prot), protection);
  }
  if (result == nullptr) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  AddRegion(s, reinterpret_cast<uintptr_t>(result), len, Region{}, origin);
  return result;
}

// MAP_PRIVATE | MAP_FIXED file pages over a plain anonymous region of ours:
// fresh pages with the file's contents (zeros past its end), then `prot`.
void* CopyFileInto(State& s, void* start, size_t len, int prot, int fd, off_t offset) {
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  LARGE_INTEGER file_length;
  if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &file_length)) {
    errno = EBADF;
    return MAP_FAILED;
  }
  if (MapAnonymous(s, start, len, PROT_READ | PROT_WRITE, MAP_FIXED) == MAP_FAILED) {
    return MAP_FAILED;
  }
  // Mark the replaced pages before copying: even a failed copy must not turn
  // file-origin pages into disposable private-anonymous pages.
  auto region = FindRegion(s, reinterpret_cast<uintptr_t>(start));
  const size_t first = (reinterpret_cast<uintptr_t>(start) - region->first) / s.page_size;
  std::fill(region->second.origins.begin() + first,
            region->second.origins.begin() + first + len / s.page_size, Origin::kPrivateFile);
  if (!CopyFromFile(s, file, static_cast<uint64_t>(file_length.QuadPart),
                    static_cast<uint64_t>(offset), static_cast<uint8_t*>(start), len)) {
    errno = EACCES;
    return MAP_FAILED;
  }
  DWORD old;
  if ((prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) != (PROT_READ | PROT_WRITE) &&
      !VirtualProtect(start, len, PageProtection(prot), &old)) {
    errno = EACCES;
    return MAP_FAILED;
  }
  if ((prot & PROT_EXEC) != 0) {
    FlushInstructionCache(GetCurrentProcess(), start, len);
  }
  return start;
}

void* MapFile(State& s, void* start, size_t len, int prot, int flags, int fd, off_t offset) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  // The public entry point has already validated page-aligned file offsets.
  if (Placeholders().available() && static_cast<uint64_t>(offset) % s.page_size == 0) {
    auto it = FindRegion(s, addr);
    if (it == s.regions.end() || it->second.group != 0 || (flags & MAP_FIXED) == 0) {
      void* result = GroupFile(s, start, len, prot, flags, fd, offset);
      if (result != MAP_FAILED || errno != 0) return result;
      // A shared mapping that ends in a partial page: a plain view.
      if (fixed) {
        errno = EINVAL;
        return MAP_FAILED;
      }
    }
  }
  if ((flags & MAP_FIXED) != 0 && (flags & MAP_FIXED_NOREPLACE) == 0 &&
      (flags & MAP_SHARED) == 0) {
    auto it = FindRegion(s, addr);
    if (it != s.regions.end() && it->second.kind == Kind::kAnon && it->second.group == 0 &&
        addr + len <= it->first + it->second.size) {
      return CopyFileInto(s, start, len, prot, fd, offset);
    }
  }
  if ((prot & PROT_EXEC) != 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  HANDLE file = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return MAP_FAILED;
  }

  size_t padding = static_cast<size_t>(offset) % s.granularity;
  uint64_t file_offset = static_cast<uint64_t>(offset) - padding;
  size_t view_length = len + padding;

  DWORD map_access;
  DWORD view_access;
  bool shared = (flags & MAP_SHARED) != 0;
  if ((prot & PROT_WRITE) != 0) {
    map_access = shared ? PAGE_READWRITE : PAGE_WRITECOPY;
    view_access = shared ? FILE_MAP_WRITE : FILE_MAP_COPY;
  } else {
    map_access = PAGE_READONLY;
    view_access = FILE_MAP_READ;
  }

  // A view may not extend past the end of the file (the mapping is the
  // file's size); length 0 maps up to the end instead (the tail of the last
  // page reads as zeros). mmap rounds the length up to whole pages, so a
  // writable shared map of a file whose size is not a page multiple (a
  // SharedMemory region) needs this too.
  LARGE_INTEGER file_length;
  if (!GetFileSizeEx(file, &file_length)) {
    errno = EACCES;
    return MAP_FAILED;
  }
  if (file_offset + view_length > static_cast<uint64_t>(file_length.QuadPart)) {
    if (file_offset >= static_cast<uint64_t>(file_length.QuadPart)) {
      errno = ENXIO;
      return MAP_FAILED;
    }
    view_length = 0;
  }

  HANDLE mapping = CreateFileMappingW(file, nullptr, map_access, 0, 0, nullptr);
  if (mapping == nullptr) {
    errno = EACCES;
    return MAP_FAILED;
  }
  DWORD high = static_cast<DWORD>(file_offset >> 32);
  DWORD low = static_cast<DWORD>(file_offset & 0xffffffffu);
  void* want = start == nullptr ? nullptr : static_cast<uint8_t*>(start) - padding;
  void* view = MapViewOfFileEx(mapping, view_access, high, low, view_length, want);
  if (view == nullptr && want != nullptr && !fixed) {
    view = MapViewOfFileEx(mapping, view_access, high, low, view_length, nullptr);
  }
  if (view == nullptr) {
    CloseHandle(mapping);
    errno = (flags & MAP_FIXED_NOREPLACE) != 0 ? EEXIST : fixed ? EINVAL : ENOMEM;
    return MAP_FAILED;
  }

  void* result = static_cast<uint8_t*>(view) + padding;
  Region region;
  region.kind = Kind::kView;
  region.view = view;
  region.mapping = mapping;
  region.shared = shared;
  region.offset = static_cast<uint64_t>(offset);
  region.view_protection = map_access;
  region.view_padding = padding;
  region.physical_extent = len + padding;
  region.shared_alias = view_length != 0 && shared && (prot == PROT_READ || prot == (PROT_READ | PROT_WRITE)) &&
                        GetFileType(file) == FILE_TYPE_DISK;
  AddRegion(s, reinterpret_cast<uintptr_t>(result), RoundUp(len, s.page_size), std::move(region),
            shared ? Origin::kSharedFile : Origin::kPrivateFile);
  return result;
}

// Calls `fn(begin, size, info)` for each run of pages in [addr, addr + len)
// that VirtualQuery reports as one (same allocation, state and protection):
// a Windows call on memory may not span allocations.
template <typename Fn>
bool ForEachRun(void* addr, size_t len, Fn fn) {
  uint8_t* p = static_cast<uint8_t*>(addr);
  uint8_t* end = p + len;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(p, &info, sizeof(info)) == 0) return false;
    uint8_t* run_end = std::min(end, static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize);
    if (!fn(p, static_cast<size_t>(run_end - p), info)) return false;
    p = run_end;
  }
  return true;
}

// Query in bounded stack batches: no per-call allocation or syscall per page.
template <typename Fn>
bool ForEachWorkingSetBatch(uintptr_t begin, size_t length, size_t page, Fn fn) {
  PSAPI_WORKING_SET_EX_INFORMATION entries[128] = {};
  for (size_t done = 0; done < length;) {
    const size_t count = std::min<size_t>(128, (length - done) / page);
    for (size_t i = 0; i < count; ++i) {
      entries[i] = {};
      entries[i].VirtualAddress = reinterpret_cast<void*>(begin + done + i * page);
    }
    if (!QueryWorkingSetEx(GetCurrentProcess(), entries,
                          static_cast<DWORD>(count * sizeof(entries[0])))) {
      gesher::FailWithLastError();
      return false;
    }
    if (!fn(entries, count)) return false;
    done += count * page;
  }
  return true;
}

// Only pass runs known to be locked. VirtualUnlock on an unlocked page
// otherwise evicts it. Drop an entry only after the native lock is gone.
bool UnlockGuestPages(State& s, uintptr_t begin, uintptr_t end) {
  auto it = s.locked_pages.lower_bound(begin);
  while (it != s.locked_pages.end() && *it < end) {
    const uintptr_t first = *it;
    uintptr_t next = first + s.page_size;
    auto last = std::next(it);
    while (last != s.locked_pages.end() && *last == next && next < end) {
      next += s.page_size;
      ++last;
    }
    if (VirtualUnlock(reinterpret_cast<void*>(first), next - first)) {
      it = s.locked_pages.erase(it, last);
      continue;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_NOT_LOCKED && error != ERROR_INVALID_ADDRESS) {
      gesher::FailWithLastError();
      return false;
    }
    // An unregistered allocation may have gone away. Query before any
    // retry so unlocked pages do not get evicted by a second native call.
    if (!ForEachWorkingSetBatch(first, next - first, s.page_size,
        [&](const auto* entries, size_t count) {
          for (size_t i = 0; i < count; ++i) {
            const auto& attributes = entries[i].VirtualAttributes;
            if (attributes.Valid && attributes.Locked &&
                !VirtualUnlock(entries[i].VirtualAddress, s.page_size)) {
              gesher::FailWithLastError();
              return false;
            }
            it = s.locked_pages.erase(it);
          }
          return true;
        })) return false;
  }
  return true;
}

// Locked pages cannot be discarded, and large pages are not supported by
// this page-granular path. mlock/munlock share the mapping registry lock.
bool AdviceUnlocked(uintptr_t begin, size_t length, size_t page) {
  return ForEachWorkingSetBatch(begin, length, page, [](const auto* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      const auto& attributes = entries[i].VirtualAttributes;
      if (attributes.Valid && (attributes.Locked || attributes.LargePage)) {
        errno = EINVAL;
        return false;
      }
    }
    return true;
  });
}

// Validate the complete logical range before any discard. A private file tail
// is still a file mapping even though Windows reports MEM_PRIVATE. A range
// ending in a hole or an unsupported mapping must not zero its earlier pages.
// Caller holds State::lock throughout preflight and execution.
bool PreflightAdvice(State& s, uintptr_t begin, size_t length, int advice) {
  const uintptr_t end = begin + length;
  const bool discard = advice == MADV_DONTNEED || advice == MADV_FREE || advice == MADV_REMOVE;
  for (uintptr_t at = begin; at < end;) {
    auto found = FindRegion(s, at);
    if (found == s.regions.end()) {
      MEMORY_BASIC_INFORMATION info;
      if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) != sizeof(info) ||
          info.State != MEM_COMMIT) {
        errno = ENOMEM;
        return false;
      }
      // Native allocations outside this registry have no certified Linux
      // provenance. Hints are harmless; destructive advice is unsupported.
      if (discard) { errno = EOPNOTSUPP; return false; }
      at = std::min(end, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
      continue;
    }
    const Region& region = found->second;
    const uintptr_t limit = std::min(end, found->first + region.size);
    for (size_t i = (at - found->first) / s.page_size;
         i < (limit - found->first) / s.page_size; ++i) {
      if (region.kind == Kind::kHole || !region.live[i]) { errno = ENOMEM; return false; }
      if (!discard) continue;
      const Origin origin = region.origins[i];
      if (advice == MADV_REMOVE) {
        errno = origin == Origin::kPrivateFile ? EACCES
              : origin == Origin::kPrivateAnonymous || origin == Origin::kDevice ? EINVAL
              : EOPNOTSUPP;
        return false;  // requires a backing-store hole operation, never memset
      }
      if (origin != Origin::kPrivateAnonymous) {
        errno = advice == MADV_FREE || origin == Origin::kDevice ? EINVAL : EOPNOTSUPP;
        return false;
      }
    }
    at = limit;
  }
  errno = ENOMEM;
  return ForEachRun(reinterpret_cast<void*>(begin), length,
      [&](uint8_t* p, size_t n, const MEMORY_BASIC_INFORMATION& info) {
        if (info.State != MEM_COMMIT && info.State != MEM_RESERVE) return false;
        if (!discard) return true;
        if (info.Type != MEM_PRIVATE) { errno = EOPNOTSUPP; return false; }
        if ((info.Protect & PAGE_GUARD) != 0) { errno = EINVAL; return false; }
        return info.State == MEM_RESERVE ||
               AdviceUnlocked(reinterpret_cast<uintptr_t>(p), n, s.page_size);
      });
}

// Linux accepts byte ranges and includes every intersecting page.
bool LockRange(const State& s, uintptr_t& begin, size_t& length) {
  const size_t offset = begin % s.page_size;
  if (length > SIZE_MAX - offset - (s.page_size - 1)) {
    errno = EINVAL;
    return false;
  }
  begin -= offset;
  length = RoundUp(length + offset, s.page_size);
  if (length > UINTPTR_MAX - begin) {
    errno = EINVAL;
    return false;
  }
  return true;
}

// A committed file view can contain logical holes. A tracked PROT_NONE
// reservation can contain live pages even without Windows commitment.
bool IsMappedRange(State& s, uintptr_t begin, size_t length) {
  const uintptr_t end = begin + length;
  for (uintptr_t at = begin; at < end;) {
    auto found = FindRegion(s, at);
    if (found != s.regions.end()) {
      const auto& region = found->second;
      const uintptr_t limit = std::min(end, found->first + region.size);
      const auto first = region.live.begin() + (at - found->first) / s.page_size;
      const auto last = region.live.begin() + (limit - found->first) / s.page_size;
      if (std::find(first, last, false) != last) return false;
      at = limit;
    } else {
      MEMORY_BASIC_INFORMATION info;
      if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0 ||
          info.State != MEM_COMMIT) return false;
      const auto next = s.regions.upper_bound(at);
      at = std::min(end, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
      if (next != s.regions.end()) at = std::min(at, next->first);
    }
  }
  return true;
}

// The native allocator can retire unregistered heap or stack pages without
// going through munmap. Their entries must not consume guest quota afterward.
bool PruneNativeLocks(State& s) {
  for (auto it = s.locked_pages.begin(); it != s.locked_pages.end();) {
    if (FindRegion(s, *it) != s.regions.end()) { ++it; continue; }
    bool locked = false;
    if (!ForEachWorkingSetBatch(*it, s.page_size, s.page_size, [&](const auto* entries, size_t) {
          const auto& attributes = entries[0].VirtualAttributes;
          locked = attributes.Valid && attributes.Locked;
          return true;
        })) return false;
    if (locked) ++it;
    else it = s.locked_pages.erase(it);
  }
  return true;
}

rlim_t GuestLockLimit() {
  struct rlimit limit;
  getrlimit(RLIMIT_MEMLOCK, &limit);
  return limit.rlim_cur;
}

bool CanAddLockedPages(const State& s, size_t additional, rlim_t byte_limit,
                       size_t locked_pages) {
  const rlim_t page_limit = byte_limit == RLIM_INFINITY ? RLIM_INFINITY : byte_limit / s.page_size;
  return locked_pages <= page_limit && additional <= page_limit - locked_pages;
}

// The caller has already allocated all accounting nodes. On failure, roll
// back earlier Win32 runs without disturbing an overlapping preexisting lock.
bool LockNewPages(State& s, std::set<uintptr_t>& pages) {
  const int previous_errno = errno;
  std::vector<std::pair<uintptr_t, uintptr_t>> acquired;
  acquired.reserve(pages.size());
  for (auto it = pages.begin(); it != pages.end();) {
    const uintptr_t first = *it++;
    uintptr_t last = first + s.page_size;
    while (it != pages.end() && *it == last) {
      last += s.page_size;
      ++it;
    }
    errno = ENOMEM;
    const bool ok = ForEachRun(reinterpret_cast<void*>(first), last - first,
        [&](uint8_t* addr, size_t length, const MEMORY_BASIC_INFORMATION& info) {
          if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            errno = ENOMEM;
            return false;
          }
          if (!VirtualLock(addr, length)) {
            errno = EAGAIN;
            return false;
          }
          const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
          acquired.emplace_back(start, start + length);
          return true;
        });
    if (!ok) {
      const int error = errno;
      for (const auto& [begin, end] : acquired) {
        if (!VirtualUnlock(reinterpret_cast<void*>(begin), end - begin)) {
          // Keep a record of any native lock we could not roll back.
          auto page = pages.lower_bound(begin);
          while (page != pages.end() && *page < end)
            s.locked_pages.insert(pages.extract(page++));
        }
      }
      errno = error;
      return false;
    }
  }
  s.locked_pages.merge(pages);
  errno = previous_errno;
  return true;
}

// --- zeroing private pages ----------------------------------------------------

// madvise(MADV_DONTNEED) of private pages decommits and recommits them, which
// leaves a moment in which they are not there at all: another thread touching
// them then (ART clears card table and bitmap ranges while mutators run) faults
// where Linux would have read zeros. Each such range is published here for as
// long as it is decommitted; the fault handler waits for it and retries
// (RetryAfterMadvise).
struct ZeroingRange {
  std::atomic<uintptr_t> begin{0};
  std::atomic<uintptr_t> end{0};
};
constexpr size_t kZeroingSlots = 64;
ZeroingRange g_zeroing[kZeroingSlots];
// Bumped after a range is published and again after it is withdrawn, so the
// handler can tell that a zeroing started or ended while it looked.
std::atomic<uint64_t> g_zeroing_epoch{0};

class Zeroing {
 public:
  Zeroing(void* p, size_t n) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    for (;;) {
      for (ZeroingRange& r : g_zeroing) {
        uintptr_t free = 0;
        if (r.begin.compare_exchange_strong(free, 1, std::memory_order_acq_rel)) {
          r.end.store(begin + n, std::memory_order_relaxed);
          r.begin.store(begin, std::memory_order_release);
          g_zeroing_epoch.fetch_add(1, std::memory_order_seq_cst);
          slot_ = &r;
          return;
        }
      }
      SwitchToThread();
    }
  }
  ~Zeroing() {
    slot_->end.store(0, std::memory_order_relaxed);
    slot_->begin.store(0, std::memory_order_release);
    g_zeroing_epoch.fetch_add(1, std::memory_order_seq_cst);
  }
  Zeroing(const Zeroing&) = delete;
  Zeroing& operator=(const Zeroing&) = delete;

 private:
  ZeroingRange* slot_ = nullptr;
};

bool BeingZeroed(uintptr_t addr) {
  for (ZeroingRange& r : g_zeroing) {
    const uintptr_t begin = r.begin.load(std::memory_order_acquire);
    if (begin != 0 && begin <= addr && addr < r.end.load(std::memory_order_relaxed)) return true;
  }
  return false;
}

// Private pages [p, p + n) of one run, committed with `protection`, as zero
// pages again.
// The recommit charges commit again; if another process took it meanwhile
// (the system commit limit), it is retried for a while: pages left
// decommitted would be an access violation nothing can retry.
bool ZeroPrivate(uint8_t* p, size_t n, DWORD protection) {
  Zeroing zeroing(p, n);
  VirtualFree(p, n, MEM_DECOMMIT);
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (VirtualAlloc(p, n, MEM_COMMIT, protection) != nullptr) return true;
    Sleep(attempt < 10 ? 1 : 50);
  }
  fprintf(stderr, "gesher-mmap: recommitting %p (0x%zx bytes) after madvise failed: %lu\n",
          static_cast<void*>(p), n, GetLastError());
  return false;
}

}  // namespace

namespace gesher {

bool RetryAfterMadvise(uintptr_t addr, ULONG_PTR access) {
  auto allowed = [&] {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0) {
      return false;
    }
    const DWORD p = info.Protect & 0xff;
    switch (access) {
      case 0:
        return p != PAGE_NOACCESS && p != PAGE_EXECUTE;
      case 1:
        return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
               p == PAGE_EXECUTE_WRITECOPY;
      case 8:
        return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
               p == PAGE_EXECUTE_WRITECOPY;
      default:
        return false;
    }
  };
  // In flight: wait for the recommit. Not in flight: the fault was a race
  // only if the page is there now - unless a zeroing started or ended while
  // we looked (the epoch moved: the page may have been recommitted and
  // decommitted again by the next madvise), then look again.
  for (;;) {
    const uint64_t epoch = g_zeroing_epoch.load(std::memory_order_seq_cst);
    const bool busy = BeingZeroed(addr);
    if (allowed()) return true;
    if (!busy && g_zeroing_epoch.load(std::memory_order_seq_cst) == epoch) return false;
    SwitchToThread();
  }
}

}  // namespace gesher

extern "C" {

// Whether mappings are placeholder-based (MAP_FIXED over file views,
// executable file views): ART's JIT asks before taking its dual view.
int gesher_mmap_placeholders(void) { return Placeholders().available() ? 1 : 0; }

// Internal outcome of the most recent mmap, not a guest libc interface.
int gesher_mmap_replaced_private_pages(void) { return g_mmap_replaced_private_pages ? 1 : 0; }

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  g_mmap_replaced_private_pages = false;
  State& s = state();
  const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  const int type = flags & 0x0f;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(addr);
  if (length == 0 ||
      (fixed && begin % s.page_size != 0) || offset < 0 ||
      static_cast<uint64_t>(offset) % s.page_size != 0 ||
      (type != MAP_PRIVATE && type != MAP_SHARED && type != MAP_SHARED_VALIDATE) ||
      (type == MAP_SHARED_VALIDATE && (flags & MAP_ANONYMOUS) != 0)) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  if (length > SIZE_MAX - (s.page_size - 1)) { errno = ENOMEM; return MAP_FAILED; }
  length = RoundUp(length, s.page_size);
  if (fixed && length > UINTPTR_MAX - begin) { errno = ENOMEM; return MAP_FAILED; }
  if ((flags & MAP_ANONYMOUS) == 0 && fd < 0) { errno = EBADF; return MAP_FAILED; }
  // An ordinary address is only a hint, and need not be page aligned.
  if (!fixed) addr = reinterpret_cast<void*>(begin - begin % s.page_size);
  std::lock_guard<std::mutex> guard(s.lock);
  void* result;
  if ((flags & MAP_ANONYMOUS) != 0) {
    result = MapAnonymous(s, addr, length, prot, flags);
  } else if (gesher::IsBinderFd(fd)) {
    // /dev/binder's receive buffer: memory binder.cc copies the process's
    // incoming transactions into, read-only to the process on Linux.
    result = MapAnonymous(s, addr, length, PROT_READ | PROT_WRITE, flags & ~MAP_SHARED);
    if (result != MAP_FAILED) {
      auto region = FindRegion(s, reinterpret_cast<uintptr_t>(result));
      const size_t first = (reinterpret_cast<uintptr_t>(result) - region->first) / s.page_size;
      std::fill(region->second.origins.begin() + first,
                region->second.origins.begin() + first + length / s.page_size, Origin::kDevice);
      gesher::SetBinderMapping(fd, result, length);
    }
  } else {
    result = MapFile(s, addr, length, prot, flags, fd, offset);
  }
  TRACE("mmap(%p, 0x%zx, prot=%d, flags=0x%x, fd=%d, offset=0x%llx) = %p\n", addr, length, prot,
        flags, fd, static_cast<unsigned long long>(offset), result);
  return result;
}

void* mmap64(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  return mmap(addr, length, prot, flags, fd, offset);
}

}  // extern "C"

namespace {

// The caller holds the registry lock, including when mremap releases either
// the old mapping or a failed destination.
int Unmap(State& s, uintptr_t begin, uintptr_t end) {
  std::vector<uintptr_t> groups;
  struct EmptyGroupsCleanup {
    State& state;
    const std::vector<uintptr_t>& groups;
    ~EmptyGroupsCleanup() {
      // A later native failure must not strand reservations emptied earlier
      // in this request. Cleanup must also preserve that failure's errno.
      const int saved_errno = errno;
      for (uintptr_t group : groups) ReleaseGroupIfEmpty(state, group);
      errno = saved_errno;
    }
  } cleanup{s, groups};
  auto it = FindRegion(s, begin);
  if (it == s.regions.end()) it = s.regions.lower_bound(begin);
  while (it != s.regions.end() && it->first < end) {
    const uintptr_t start = it->first;
    Region& region = it->second;
    const uintptr_t from = std::max(begin, start);
    const uintptr_t to = std::min(end, start + region.size);
    if (region.group != 0 &&
        std::find(groups.begin(), groups.end(), region.group) == groups.end()) {
      groups.push_back(region.group);
    }
    if (region.kind == Kind::kHole) { ++it; continue; }
    if (region.group != 0 && region.kind == Kind::kAnon) {
      if (!VirtualFree(reinterpret_cast<void*>(from), to - from,
                       MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        return gesher::FailWithLastError();
      }
      ForgetLocks(s, from, to);
      if (it->first != from) it = SplitEntry(s, it, from);
      if (it->first + it->second.size != to) SplitEntry(s, it, to);
      Region& hole = it->second;
      hole.kind = Kind::kHole;
      std::fill(hole.live.begin(), hole.live.end(), false);
      hole.live_pages = 0;
      ++it;
      continue;
    }

    const auto first = region.live.begin() + (from - start) / s.page_size;
    const auto last = region.live.begin() + (to - start) / s.page_size;
    const auto removed = static_cast<size_t>(std::count(first, last, true));
    if (removed == 0) { ++it; continue; }
    if (removed == region.live_pages) {
      bool released;
      if (region.kind == Kind::kAnon) {
        released = VirtualFree(reinterpret_cast<void*>(start), 0, MEM_RELEASE) != 0;
      } else if (region.group == 0) {
        released = UnmapViewOfFile(region.view) != 0;
      } else {
        released = Placeholders().unmap2(GetCurrentProcess(), region.view,
                                         MEM_PRESERVE_PLACEHOLDER) != 0;
      }
      if (!released) return gesher::FailWithLastError();
      ForgetLocks(s, start, start + region.size);
      if (region.kind == Kind::kView) CloseHandle(region.mapping);
      if (region.group == 0) {
        it = s.regions.erase(it);
        continue;
      }
      region.kind = Kind::kHole;
      region.view = nullptr;
      region.mapping = nullptr;
    } else if (region.kind == Kind::kAnon) {
      if (!VirtualFree(reinterpret_cast<void*>(from), to - from, MEM_DECOMMIT))
        return gesher::FailWithLastError();
      ForgetLocks(s, from, to);
    } else {
      // A partial unmap leaves the Win32 view alive. Make the range
      // inaccessible before retiring locks, so a failed protect keeps them.
      auto* address = reinterpret_cast<uint8_t*>(from);
      struct SavedProtection { uint8_t* address; size_t size; DWORD value; };
      std::vector<SavedProtection> saved;
      std::set<uintptr_t> originally_locked(s.locked_pages.lower_bound(from),
                                            s.locked_pages.lower_bound(to));
      if (!originally_locked.empty() &&
          !ForEachRun(address, to - from, [&](uint8_t* page, size_t size,
                                              const MEMORY_BASIC_INFORMATION& info) {
            saved.push_back({page, size, info.Protect});
            return true;
          })) return gesher::FailWithLastError();
      DWORD old;
      if (!VirtualProtect(address, to - from, PAGE_NOACCESS, &old))
        return gesher::FailWithLastError();
      if (!originally_locked.empty() && !UnlockGuestPages(s, from, to)) {
        const int error = errno;
        for (const auto& span : saved)
          VirtualProtect(span.address, span.size, span.value, &old);
        for (auto page = s.locked_pages.lower_bound(from);
             page != s.locked_pages.end() && *page < to; ++page) {
          originally_locked.erase(*page);
        }
        LockNewPages(s, originally_locked);
        errno = error;
        return -1;
      }
    }
    region.shared_alias = false;
    std::fill(first, last, false);
    region.live_pages -= removed;
    ++it;
  }
  return 0;
}

}  // namespace

extern "C" {

int munmap(void* addr, size_t length) {
  State& s = state();
  const uintptr_t begin = reinterpret_cast<uintptr_t>(addr);
  if (length == 0 || !AdviceRange(begin, length, s.page_size, &length)) {
    errno = EINVAL;
    return -1;
  }
  TRACE("munmap(%p, 0x%zx)\n", addr, length);
  std::lock_guard<std::mutex> guard(s.lock);
  return Unmap(s, begin, begin + length);
}

int mprotect(void* addr, size_t length, int prot) {
  TRACE("mprotect(%p, 0x%zx, %d)\n", addr, length, prot);
  if (length == 0) return 0;
  State& s = state();
  std::lock_guard<std::mutex> registry_guard(s.lock);
  InvalidateSharedAliases(s, reinterpret_cast<uintptr_t>(addr), length);
  const DWORD protection = PageProtection(prot);
  const bool ok = ForEachRun(addr, length, [&](uint8_t* p, size_t n, const MEMORY_BASIC_INFORMATION& info) {
    // Copy-on-write views stay copy-on-write when made writable. A private
    // view mapped read-only (GroupFile) has a read-only AllocationProtect:
    // the registry says it is private.
    DWORD want = protection;
    if (info.Type == MEM_MAPPED) {
      bool private_view = info.AllocationProtect == PAGE_WRITECOPY ||
                          info.AllocationProtect == PAGE_EXECUTE_WRITECOPY;
      if (!private_view) {
        auto region = FindRegion(s, reinterpret_cast<uintptr_t>(p));
        private_view = region != s.regions.end() && region->second.kind == Kind::kView &&
                       !region->second.shared;
      }
      if (private_view) want = CopyOnWrite(want);
    }
    // Reserved, not committed: PROT_NONE anonymous memory of ours
    // (AnonymousCommit), or a thread stack's pages below its committed part.
    // Ours already fault on any access, so PROT_NONE keeps them reserved; any
    // other protection commits them (zero-filled, as they read on Linux). A
    // stack's pages are committed even for PROT_NONE: ART protects the bottom
    // of each stack for its implicit stack overflow checks, and GrowStack
    // (fault_signals.cc) stops at committed pages.
    if (info.State == MEM_RESERVE) {
      if (want == PAGE_NOACCESS) {
        auto region = FindRegion(s, reinterpret_cast<uintptr_t>(p));
        if (region != s.regions.end() && region->second.kind == Kind::kAnon) return true;
      }
      return VirtualAlloc(p, n, MEM_COMMIT, want) != nullptr;
    }
    DWORD old;
    return VirtualProtect(p, n, want, &old) != 0;
  });
  if (!ok) {
    errno = EACCES;
    return -1;
  }
  return 0;
}

int madvise(void* addr, size_t length, int advice) {
  TRACE("madvise(%p, 0x%zx, %d)\n", addr, length, advice);
  switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
    case MADV_DONTNEED:
    case MADV_FREE:
    case MADV_REMOVE:
    case MADV_DONTFORK:
    case MADV_DOFORK:
    case MADV_HUGEPAGE:
    case MADV_NOHUGEPAGE:
      break;
    default:
      errno = EINVAL;
      return -1;
  }
  State& s = state();
  if (!AdviceRange(reinterpret_cast<uintptr_t>(addr), length, s.page_size, &length)) return -1;
  if (length == 0) return 0;
  // Keep mprotect/mremap/MAP_FIXED from observing the temporary decommit
  // state before recommit restores protection. Loads use RetryAfterMadvise.
  std::lock_guard<std::mutex> registry_guard(s.lock);
  const int previous_errno = errno;
  if (advice == MADV_DONTNEED || advice == MADV_FREE || advice == MADV_REMOVE) {
    const auto begin = reinterpret_cast<uintptr_t>(addr);
    auto pinned = s.locked_pages.lower_bound(begin);
    if (pinned != s.locked_pages.end() && *pinned < begin + length)
      return errno = EINVAL, -1;
  }
  if (!PreflightAdvice(s, reinterpret_cast<uintptr_t>(addr), length, advice)) return -1;
  if (advice == MADV_DONTNEED || advice == MADV_FREE) {
    // Eager reclaim is permitted for MADV_FREE. Only certified private
    // anonymous pages reach here, including currently reserved PROT_NONE.
    if (!ForEachRun(addr, length, [](uint8_t* p, size_t n, const MEMORY_BASIC_INFORMATION& info) {
          return info.State != MEM_COMMIT || ZeroPrivate(p, n, info.Protect);
        })) {
      errno = ENOMEM;
      return -1;
    }
  } else if (advice == MADV_WILLNEED) {
    if (!ForEachRun(addr, length, [](uint8_t* p, size_t n, const MEMORY_BASIC_INFORMATION& info) {
          // A hint must not turn an inaccessible reservation into a committed
          // mapping. Prefetch currently accessible runs only.
          if (info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS) return true;
          WIN32_MEMORY_RANGE_ENTRY range = {p, n};
          if (PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0)) return true;
          gesher::FailWithLastError();
          return false;
        })) return -1;
  }
  // NORMAL/RANDOM/SEQUENTIAL and huge-page preferences are validated hints.
  // DONTFORK/DOFORK need no inheritance change while fork is unsupported;
  // ART uses DONTFORK for its shared JIT views.
  errno = previous_errno;
  return 0;
}

// As Linux: the address must be page-aligned and the flags valid; a range
// with unmapped pages is ENOMEM; file views are flushed, and anonymous or
// private memory (nothing to write back) succeeds. UPX's unpacker msyncs the
// copy of a memfd it maps over a library's text.
int msync(void* addr, size_t length, int flags) {
  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  if ((start & (state().page_size - 1)) != 0 ||
      (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
      (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC)) {
    errno = EINVAL;
    return -1;
  }
  // Linux synchronizes whole pages and rejects an address range whose end
  // wraps. Keep its unsigned length rounding, including a zero rounded span.
  length = RoundUp(length, state().page_size);
  const uintptr_t end = start + length;
  if (end < start) {
    errno = ENOMEM;
    return -1;
  }
  for (uintptr_t at = start; at < end;) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT) {
      errno = ENOMEM;
      return -1;
    }
    const uintptr_t run_end =
        std::min(end, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
    if (info.Type == MEM_MAPPED &&
        !FlushViewOfFile(reinterpret_cast<void*>(at), static_cast<SIZE_T>(run_end - at))) {
      errno = EINVAL;
      return -1;
    }
    at = run_end;
  }
  return 0;
}

int mincore(void* addr, size_t length, unsigned char* vec) {
  size_t page = state().page_size;
  uint8_t* p = static_cast<uint8_t*>(addr);
  for (size_t i = 0; i < (length + page - 1) / page; ++i) {
    MEMORY_BASIC_INFORMATION info;
    vec[i] = VirtualQuery(p + i * page, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT;
  }
  return 0;
}

int mlock(const void* addr, size_t length) {
  State& s = state();
  std::lock_guard<std::mutex> registry_guard(s.lock);
  // Like ordinary Android application processes, this layer has no
  // CAP_IPC_LOCK. A zero soft limit denies the operation outright.
  const rlim_t limit = GuestLockLimit();
  if (limit == 0) return errno = EPERM, -1;
  uintptr_t begin = reinterpret_cast<uintptr_t>(addr);
  if (!LockRange(s, begin, length)) return -1;
  if (length == 0) return 0;
  if (!PruneNativeLocks(s)) return -1;
  const uintptr_t end = begin + length;
  const auto first = s.locked_pages.lower_bound(begin);
  const auto last = s.locked_pages.lower_bound(end);
  const size_t overlapping = static_cast<size_t>(std::distance(first, last));
  if (!CanAddLockedPages(s, length / s.page_size - overlapping, limit, s.locked_pages.size()))
    return errno = ENOMEM, -1;
  if (!IsMappedRange(s, begin, length)) return errno = ENOMEM, -1;

  std::set<uintptr_t> pages;
  auto locked = first;
  for (uintptr_t at = begin; at < end; at += s.page_size) {
    if (locked != last && *locked == at) ++locked;
    else pages.insert(at);
  }
  if (!LockNewPages(s, pages)) return -1;
  InvalidateSharedAliases(s, begin, length);
  return 0;
}

int munlock(const void* addr, size_t length) {
  State& s = state();
  uintptr_t begin = reinterpret_cast<uintptr_t>(addr);
  if (!LockRange(s, begin, length)) return -1;
  if (length == 0) return 0;
  std::lock_guard<std::mutex> registry_guard(s.lock);
  if (!IsMappedRange(s, begin, length)) return errno = ENOMEM, -1;
  if (!PruneNativeLocks(s)) return -1;
  return UnlockGuestPages(s, begin, begin + length) ? 0 : -1;
}

int munlockall(void) {
  State& s = state();
  std::lock_guard<std::mutex> registry_guard(s.lock);
  if (!PruneNativeLocks(s)) return -1;
  return UnlockGuestPages(s, 0, UINTPTR_MAX) ? 0 : -1;
}

// mremap for this layer's anonymous mappings. ART's CMC collector, without
// userfaultfd, moves its whole space aside (MREMAP_MAYMOVE | MREMAP_FIXED,
// often with MREMAP_DONTUNMAP) before compacting it back. Windows cannot move
// pages to another address, so moving copies them: the destination gets the
// source's contents and protection, and the source is unmapped, or with
// MREMAP_DONTUNMAP stays mapped and reads back as zeros (private anonymous
// memory). Shrinking in place unmaps the tail; growing in place fails with
// ENOMEM, as on Linux when the next pages are taken. File views cannot move.
void* mremap(void* old_address, size_t old_size, size_t new_size, int flags, ...) {
  void* fixed_address = nullptr;
  if ((flags & MREMAP_FIXED) != 0) {
    va_list args;
    va_start(args, flags);
    fixed_address = va_arg(args, void*);
    va_end(args);
  }
  State& s = state();
  TRACE("mremap(%p, 0x%zx, 0x%zx, flags=0x%x, %p)\n", old_address, old_size, new_size, flags,
        fixed_address);
  uintptr_t old_begin = reinterpret_cast<uintptr_t>(old_address);
  bool may_move = (flags & MREMAP_MAYMOVE) != 0;
  bool dont_unmap = (flags & MREMAP_DONTUNMAP) != 0;
  if (old_begin % s.page_size != 0 || new_size == 0 ||
      (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP)) != 0 ||
      ((flags & MREMAP_FIXED) != 0 && !may_move) || (dont_unmap && !may_move) ||
      (dont_unmap && old_size != new_size)) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  // Validate additions before rounding. Existing anonymous algorithm follows
  // unchanged; Slice A dispatch is limited to owned nonanonymous file views.
  if (old_size > SIZE_MAX - (s.page_size - 1) || new_size > SIZE_MAX - (s.page_size - 1)) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  old_size = RoundUp(old_size, s.page_size);
  new_size = RoundUp(new_size, s.page_size);
  if (old_size > UINTPTR_MAX - old_begin ||
      new_size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(fixed_address)) {
    errno = EINVAL;
    return MAP_FAILED;
  }

  std::lock_guard<std::mutex> guard(s.lock);
  auto source_region = FindRegion(s, old_begin);
  if (source_region != s.regions.end() && source_region->second.kind == Kind::kView)
    return SharedAlias(s, old_begin, old_size, new_size, flags);
  if (old_size == 0) { errno = EINVAL; return MAP_FAILED; }
  // The source: live pages of anonymous regions.
  for (uintptr_t at = old_begin; at < old_begin + old_size;) {
    auto source = FindRegion(s, at);
    if (source == s.regions.end() || source->second.kind == Kind::kHole) {
      errno = EFAULT;
      return MAP_FAILED;
    }
    if (source->second.kind != Kind::kAnon) {
      errno = EINVAL;  // a file view stays where Windows put it
      return MAP_FAILED;
    }
    const uintptr_t region_end = source->first + source->second.size;
    for (size_t i = (at - source->first) / s.page_size,
                n = (std::min(region_end, old_begin + old_size) - source->first) / s.page_size;
         i < n; ++i) {
      if (!source->second.live[i]) {
        errno = EFAULT;
        return MAP_FAILED;
      }
      if (source->second.origins[i] != Origin::kPrivateAnonymous) {
        errno = EOPNOTSUPP;  // copying would lose file/shared/device provenance
        return MAP_FAILED;
      }
    }
    at = region_end;
  }

  // The destination: fresh pages, at fixed_address or anywhere.
  uintptr_t new_begin = reinterpret_cast<uintptr_t>(fixed_address);
  if (fixed_address != nullptr) {
    if (new_begin % s.page_size != 0 ||
        (new_begin < old_begin + old_size && old_begin < new_begin + new_size)) {
      errno = EINVAL;  // the ranges may not overlap
      return MAP_FAILED;
    }
  }
  const size_t copy_size = std::min(old_size, new_size);
  const auto lock_first = s.locked_pages.lower_bound(old_begin);
  const auto lock_last = s.locked_pages.lower_bound(old_begin + old_size);
  const size_t source_locks = static_cast<size_t>(std::distance(lock_first, lock_last));
  // The existing DONTUNMAP path decommits and recommits the old mapping.
  // It cannot roll that operation back to the original pinned pages.
  if (dont_unmap && source_locks != 0) return errno = EOPNOTSUPP, MAP_FAILED;
  const bool growing_locked = new_size > old_size && source_locks != 0;
  if (growing_locked) {
    // A change in lock state splits a Linux VMA. There is no single
    // lock policy to inherit when a resize crosses that boundary.
    if (source_locks != old_size / s.page_size) return errno = EFAULT, MAP_FAILED;
    if (!PruneNativeLocks(s)) return MAP_FAILED;
    size_t replaced_locks = 0;
    if (fixed_address != nullptr) {
      replaced_locks = static_cast<size_t>(std::distance(s.locked_pages.lower_bound(new_begin),
          s.locked_pages.lower_bound(new_begin + new_size)));
    }
    if (!CanAddLockedPages(s, (new_size - old_size) / s.page_size, GuestLockLimit(),
                           s.locked_pages.size() - replaced_locks)) {
      return errno = EAGAIN, MAP_FAILED;
    }
  }
  // In place, unless asked to move: MREMAP_FIXED and MREMAP_DONTUNMAP always
  // move, MREMAP_MAYMOVE alone only to grow.
  if (!may_move || (!dont_unmap && (flags & MREMAP_FIXED) == 0 && new_size <= old_size)) {
    if (new_size > old_size) {
      errno = ENOMEM;  // growing in place is not supported
      return MAP_FAILED;
    }
    if (new_size < old_size && Unmap(s, old_begin + new_size, old_begin + old_size) != 0)
      return MAP_FAILED;
    return old_address;
  }

  // Retiring a locked source must be a single native unmap. If a later
  // region failed to unmap, the already retired regions could not be restored.
  if (source_locks != 0 &&
      old_begin + old_size > source_region->first + source_region->second.size) {
    return errno = EFAULT, MAP_FAILED;
  }
  std::vector<size_t> locked_offsets;
  for (auto it = s.locked_pages.lower_bound(old_begin);
       it != s.locked_pages.end() && *it - old_begin < copy_size; ++it) {
    locked_offsets.push_back(*it - old_begin);
  }

  void* destination = MapAnonymous(s, fixed_address, new_size, PROT_READ | PROT_WRITE,
                                   fixed_address != nullptr ? MAP_FIXED : 0);
  if (destination == MAP_FAILED) return MAP_FAILED;
  new_begin = reinterpret_cast<uintptr_t>(destination);
  const auto rollback_destination = [&]() {
    const int error = errno;
    Unmap(s, new_begin, new_begin + new_size);
    errno = error;
    return MAP_FAILED;
  };
  if (fixed_address != nullptr && destination != fixed_address) {
    errno = EINVAL;
    return rollback_destination();
  }

  // Copy the source with its protections. The new pages stay accessible
  // until their native locks have been installed.
  auto* from = static_cast<uint8_t*>(old_address);
  auto* to = static_cast<uint8_t*>(destination);
  struct ProtectionRun { size_t offset; size_t size; DWORD protection; };
  std::vector<ProtectionRun> protections;
  errno = ENOMEM;
  if (!ForEachRun(from, copy_size, [&](uint8_t* addr, size_t length,
                                      const MEMORY_BASIC_INFORMATION& info) {
    const size_t offset = addr - from;
    if (info.State == MEM_RESERVE) {
      return VirtualFree(to + offset, length, MEM_DECOMMIT) != 0;
    }
    if (info.State != MEM_COMMIT) return false;
    const DWORD protection = info.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    const bool readable = protection != PAGE_NOACCESS && protection != PAGE_EXECUTE;
    DWORD previous;
    if (!readable && !VirtualProtect(addr, length, PAGE_READONLY, &previous)) return false;
    memcpy(to + offset, addr, length);
    if (!readable && !VirtualProtect(addr, length, previous, &previous)) return false;
    protections.push_back({offset, length, protection});
    return true;
  })) return rollback_destination();

  if (!locked_offsets.empty()) {
    std::set<uintptr_t> destination_locks;
    for (size_t offset : locked_offsets) destination_locks.insert(new_begin + offset);
    if (growing_locked) {
      for (size_t offset = old_size; offset < new_size; offset += s.page_size)
        destination_locks.insert(new_begin + offset);
    }
    if (!LockNewPages(s, destination_locks)) return rollback_destination();
  }
  if (growing_locked) {
    if (protections.empty()) {
      errno = ENOMEM;
      return rollback_destination();
    }
    const DWORD last_protection = protections.back().protection;
    protections.push_back({old_size, new_size - old_size, last_protection});
  }
  for (const auto& run : protections) {
    DWORD previous;
    if (!VirtualProtect(to + run.offset, run.size, run.protection, &previous)) {
      gesher::FailWithLastError();
      return rollback_destination();
    }
    if ((run.protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) != 0) {
      FlushInstructionCache(GetCurrentProcess(), to + run.offset, run.size);
    }
  }

  if (dont_unmap) {
    // The old range stays mapped with its original protections.
    ForEachRun(from, old_size, [](uint8_t* addr, size_t length,
                                 const MEMORY_BASIC_INFORMATION& info) {
      if (info.State == MEM_COMMIT) ZeroPrivate(addr, length, info.Protect);
      return true;
    });
  } else if (Unmap(s, old_begin, old_begin + old_size) != 0) {
    return rollback_destination();
  }
  return destination;
}

}  // extern "C"
