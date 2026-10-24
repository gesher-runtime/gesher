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

#include <errno.h>
#include <io.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "binder_internal.h"

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
  bool shared = false;        // views: MAP_SHARED
  std::vector<bool> live;     // one entry per page
  size_t live_pages = 0;
};

struct State {
  std::mutex lock;
  std::map<uintptr_t, Region> regions;  // keyed by region start
  std::map<uintptr_t, size_t> groups;   // placeholder reservations: start, size
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
                                                Region region) {
  region.size = size;
  const bool live = region.kind != Kind::kHole;
  region.live.assign(size / s.page_size, live);
  region.live_pages = live ? region.live.size() : 0;
  return s.regions.emplace(start, std::move(region)).first;
}

void Release(uintptr_t start, Region& region) {
  if (region.kind == Kind::kAnon) {
    VirtualFree(reinterpret_cast<void*>(start), 0, MEM_RELEASE);
  } else {
    UnmapViewOfFile(region.view);
    CloseHandle(region.mapping);
  }
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
  upper.size = lower.size - (at - it->first);
  upper.live.assign(lower.live.begin() + lower_pages, lower.live.end());
  upper.live_pages = static_cast<size_t>(std::count(upper.live.begin(), upper.live.end(), true));
  upper.offset = lower.offset + (at - it->first);
  lower.size = at - it->first;
  lower.live.resize(lower_pages);
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
  Placeholders().unmap2(GetCurrentProcess(), view.view, MEM_PRESERVE_PLACEHOLDER);
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
        VirtualFree(reinterpret_cast<void*>(x), y - x, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
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
          Placeholders().unmap2(GetCurrentProcess(), it->second.view, MEM_PRESERVE_PLACEHOLDER);
          CloseHandle(it->second.mapping);
          Region& r = it->second;
          r.kind = Kind::kHole;
          r.view = nullptr;
          r.mapping = nullptr;
          std::fill(r.live.begin(), r.live.end(), false);
          r.live_pages = 0;
        } else {
          PunchView(s, it, x, y);
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
    first->second.live_pages = 0;
  }
  return first;
}

// Fills the hole `it` with anonymous zero pages.
bool FillAnonymous(std::map<uintptr_t, Region>::iterator it, int prot) {
  if (Placeholders().alloc2(GetCurrentProcess(), reinterpret_cast<void*>(it->first),
                            it->second.size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                            PageProtection(prot), nullptr, 0) == nullptr) {
    return false;
  }
  Region& r = it->second;
  r.kind = Kind::kAnon;
  std::fill(r.live.begin(), r.live.end(), true);
  r.live_pages = r.live.size();
  return true;
}

// Anonymous memory in a group: at `start` (MAP_FIXED over the group's pages,
// or into free address space), or anywhere.
void* GroupAnonymous(State& s, void* start, size_t len, int prot, int flags) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  const bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
  std::map<uintptr_t, Region>::iterator hole;
  if ((flags & MAP_FIXED) != 0 && CoveringGroup(s, addr, addr + len) != 0) {
    hole = Punch(s, addr, addr + len);
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
  if (!FillAnonymous(hole, prot)) {
    ReleaseGroupIfEmpty(s, group);
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return reinterpret_cast<void*>(result);
}

// The file's bytes from `offset` into [dest, dest + len) (zeros past its end),
// through a temporary view (the descriptor's file position stays).
bool CopyFromFile(State& s, HANDLE file, uint64_t file_size, uint64_t offset, uint8_t* dest,
                  size_t len) {
  if (offset >= file_size) return true;
  const size_t available = static_cast<size_t>(std::min<uint64_t>(len, file_size - offset));
  const size_t padding = static_cast<size_t>(offset % s.granularity);
  const uint64_t view_offset = offset - padding;
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  void* view = mapping == nullptr ? nullptr
      : MapViewOfFile(mapping, FILE_MAP_READ, static_cast<DWORD>(view_offset >> 32),
                      static_cast<DWORD>(view_offset & 0xffffffffu), available + padding);
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
  // (A private mapping of a file that cannot be reopened so is copied.)
  size_t in_view = in_file;
  HANDLE mapping = nullptr;
  ULONG view_protection = 0;
  HANDLE reopened = nullptr;
  if (in_view != 0 && exec) {
    reopened = ReOpenFile(file, GENERIC_READ | GENERIC_EXECUTE | (shared && write ? GENERIC_WRITE : 0),
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (reopened == INVALID_HANDLE_VALUE) {
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
      section = exec ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
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
  if ((flags & MAP_FIXED) != 0 && CoveringGroup(s, addr, addr + len) != 0) {
    hole = Punch(s, addr, addr + len);
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
    if (!MapViewInto(mapping, result, in_view, begin, view_protection)) {
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
    std::fill(r.live.begin(), r.live.end(), true);
    r.live_pages = r.live.size();
    DWORD protection = PageProtection(prot);
    if (!shared) protection = CopyOnWrite(protection);
    DWORD old;
    if (protection != view_protection &&
        !VirtualProtect(reinterpret_cast<void*>(result), in_view, protection, &old)) {
      return fail(EACCES);
    }
  }
  // [result + in_view, result + len): anonymous, with what the file has there.
  if (in_view < len) {
    auto tail = FindRegion(s, result + in_view);
    if (!FillAnonymous(tail, PROT_READ | PROT_WRITE)) return fail(ENOMEM);
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
  if (Placeholders().available()) {
    // MAP_FIXED over a plain region (made before placeholders were used, or
    // by the binder layer) falls through to the plain path.
    const uintptr_t addr = reinterpret_cast<uintptr_t>(start);
    auto it = FindRegion(s, addr);
    if (it == s.regions.end() || it->second.group != 0 || (flags & MAP_FIXED) == 0) {
      return GroupAnonymous(s, start, len, prot, flags);
    }
  }
  DWORD protection = PageProtection(prot);
  uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;

  // MAP_FIXED inside one of our anonymous regions: replace those pages with
  // fresh zero pages, as mmap does.
  if ((flags & MAP_FIXED) != 0) {
    auto it = FindRegion(s, addr);
    if (it != s.regions.end() && it->second.kind == Kind::kAnon &&
        addr + len <= it->first + it->second.size) {
      VirtualFree(start, len, MEM_DECOMMIT);
      if (VirtualAlloc(start, len, MEM_COMMIT, protection) == nullptr) {
        errno = ENOMEM;
        return MAP_FAILED;
      }
      Region& region = it->second;
      for (size_t i = (addr - it->first) / s.page_size, n = i + len / s.page_size; i < n; ++i) {
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
    result = VirtualAlloc(start, len, MEM_RESERVE | MEM_COMMIT, protection);
    if (result == nullptr && fixed) {
      errno = (flags & MAP_FIXED_NOREPLACE) != 0 ? EEXIST : EINVAL;
      return MAP_FAILED;
    }
  }
  if (result == nullptr) {
    result = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, protection);
  }
  if (result == nullptr) {
    errno = ENOMEM;
    return MAP_FAILED;
  }
  AddRegion(s, reinterpret_cast<uintptr_t>(result), len, Region{});
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
  // An offset that is not page-aligned (Linux refuses it) gets a plain view.
  if (Placeholders().available() && static_cast<uint64_t>(offset) % s.page_size == 0) {
    auto it = FindRegion(s, addr);
    if (it == s.regions.end() || it->second.group != 0 || (flags & MAP_FIXED) == 0) {
      void* result = GroupFile(s, start, len, prot, flags, fd, offset);
      if (result != MAP_FAILED || errno != 0) return result;
      // A shared mapping that ends in a partial page: a plain view.
      if ((flags & MAP_FIXED) != 0) {
        errno = EINVAL;
        return MAP_FAILED;
      }
    }
  }
  if ((flags & MAP_FIXED) != 0 && (flags & MAP_SHARED) == 0) {
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
  if (view == nullptr && want != nullptr && (flags & MAP_FIXED) == 0) {
    view = MapViewOfFileEx(mapping, view_access, high, low, view_length, nullptr);
  }
  if (view == nullptr) {
    CloseHandle(mapping);
    errno = (flags & MAP_FIXED) != 0 ? EINVAL : ENOMEM;
    return MAP_FAILED;
  }

  void* result = static_cast<uint8_t*>(view) + padding;
  Region region;
  region.kind = Kind::kView;
  region.view = view;
  region.mapping = mapping;
  region.shared = shared;
  AddRegion(s, reinterpret_cast<uintptr_t>(result), RoundUp(len, s.page_size), std::move(region));
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

}  // namespace

extern "C" {

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
  State& s = state();
  if (length == 0 || reinterpret_cast<uintptr_t>(addr) % s.page_size != 0 || offset < 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  length = RoundUp(length, s.page_size);
  std::lock_guard<std::mutex> guard(s.lock);
  void* result;
  if ((flags & MAP_ANONYMOUS) != 0 || fd == -1) {
    result = MapAnonymous(s, addr, length, prot, flags);
  } else if (gesher::IsBinderFd(fd)) {
    // /dev/binder's receive buffer: memory binder.cc copies the process's
    // incoming transactions into, read-only to the process on Linux.
    result = MapAnonymous(s, addr, length, PROT_READ | PROT_WRITE, flags & ~MAP_SHARED);
    if (result != MAP_FAILED) gesher::SetBinderMapping(fd, result, length);
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

int munmap(void* addr, size_t length) {
  State& s = state();
  uintptr_t begin = reinterpret_cast<uintptr_t>(addr);
  if (begin % s.page_size != 0 || length == 0) {
    errno = EINVAL;
    return -1;
  }
  uintptr_t end = begin + RoundUp(length, s.page_size);
  TRACE("munmap(%p, 0x%zx)\n", addr, length);

  std::lock_guard<std::mutex> guard(s.lock);
  std::vector<uintptr_t> groups;
  auto it = FindRegion(s, begin);
  if (it == s.regions.end()) it = s.regions.lower_bound(begin);
  while (it != s.regions.end() && it->first < end) {
    uintptr_t region_begin = it->first;
    Region& region = it->second;
    uintptr_t from = std::max(begin, region_begin);
    uintptr_t to = std::min(end, region_begin + region.size);
    if (region.group != 0) {
      if (std::find(groups.begin(), groups.end(), region.group) == groups.end()) {
        groups.push_back(region.group);
      }
      if (region.kind == Kind::kAnon) {
        // The pages go back to the placeholder.
        VirtualFree(reinterpret_cast<void*>(from), to - from,
                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        if (it->first != from) it = SplitEntry(s, it, from);
        if (it->first + it->second.size != to) SplitEntry(s, it, to);
        Region& r = it->second;
        r.kind = Kind::kHole;
        std::fill(r.live.begin(), r.live.end(), false);
        r.live_pages = 0;
        ++it;
        continue;
      }
      if (region.kind == Kind::kHole) {
        ++it;
        continue;
      }
    }
    if (from < to) {
      for (size_t i = (from - region_begin) / s.page_size, n = (to - region_begin) / s.page_size;
           i < n; ++i) {
        if (region.live[i]) {
          region.live[i] = false;
          --region.live_pages;
        }
      }
      if (region.live_pages == 0) {
        if (region.group != 0) {
          // A view nothing is left of: its placeholder.
          Placeholders().unmap2(GetCurrentProcess(), region.view, MEM_PRESERVE_PLACEHOLDER);
          CloseHandle(region.mapping);
          region.kind = Kind::kHole;
          region.view = nullptr;
          region.mapping = nullptr;
          ++it;
          continue;
        }
        Release(region_begin, region);
        it = s.regions.erase(it);
        continue;
      }
      if (region.kind == Kind::kAnon) {
        VirtualFree(reinterpret_cast<void*>(from), to - from, MEM_DECOMMIT);
      } else {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(from), to - from, PAGE_NOACCESS, &old);
      }
    }
    ++it;
  }
  for (uintptr_t group : groups) ReleaseGroupIfEmpty(s, group);
  return 0;
}

int mprotect(void* addr, size_t length, int prot) {
  TRACE("mprotect(%p, 0x%zx, %d)\n", addr, length, prot);
  if (length == 0) return 0;
  const DWORD protection = PageProtection(prot);
  const bool ok = ForEachRun(addr, length, [&](uint8_t* p, size_t n, const MEMORY_BASIC_INFORMATION& info) {
    // Copy-on-write views stay copy-on-write when made writable.
    DWORD want = protection;
    if (info.Type == MEM_MAPPED &&
        (info.AllocationProtect == PAGE_WRITECOPY ||
         info.AllocationProtect == PAGE_EXECUTE_WRITECOPY)) {
      want = CopyOnWrite(want);
    }
    DWORD old;
    if (VirtualProtect(p, n, want, &old)) return true;
    // Reserved, not committed: a thread stack's pages below its committed
    // part (mmap always commits). ART protects the bottom of each stack for
    // its implicit stack overflow checks (fault_signals.cc).
    return info.State == MEM_RESERVE && VirtualAlloc(p, n, MEM_COMMIT, want) != nullptr;
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
    case MADV_DONTNEED:
    case MADV_FREE:
    case MADV_REMOVE: {
      // Linux semantics for private anonymous memory: the range reads back as
      // zeros. Decommit + recommit gives exactly that.
      bool ok = ForEachRun(addr, length, [](uint8_t* p, size_t n, const MEMORY_BASIC_INFORMATION& info) {
        if (info.Type == MEM_PRIVATE && info.State == MEM_COMMIT) {
          VirtualFree(p, n, MEM_DECOMMIT);
          return VirtualAlloc(p, n, MEM_COMMIT, info.Protect) != nullptr;
        }
        if (info.Type == MEM_MAPPED && (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                                        PAGE_EXECUTE_READWRITE |
                                                        PAGE_EXECUTE_WRITECOPY))) {
          memset(p, 0, n);
        }
        return true;
      });
      if (!ok) {
        errno = ENOMEM;
        return -1;
      }
      return 0;
    }
    case MADV_WILLNEED: {
      WIN32_MEMORY_RANGE_ENTRY range = {addr, length};
      PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
      return 0;
    }
    default:
      return 0;  // advisory only
  }
}

int msync(void* addr, size_t length, int flags) {
  (void)flags;
  if (!FlushViewOfFile(addr, length)) {
    errno = EINVAL;
    return -1;
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
  return VirtualLock(const_cast<void*>(addr), length) ? 0 : (errno = ENOMEM, -1);
}

int munlock(const void* addr, size_t length) {
  return VirtualUnlock(const_cast<void*>(addr), length) ? 0 : (errno = ENOMEM, -1);
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
  old_size = RoundUp(old_size, s.page_size);
  new_size = RoundUp(new_size, s.page_size);

  std::unique_lock<std::mutex> guard(s.lock);
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
    }
    at = region_end;
  }

  // In place, unless asked to move: MREMAP_FIXED and MREMAP_DONTUNMAP always
  // move, MREMAP_MAYMOVE alone only to grow.
  if (!may_move || (!dont_unmap && (flags & MREMAP_FIXED) == 0 && new_size <= old_size)) {
    if (new_size > old_size) {
      errno = ENOMEM;  // growing in place is not supported
      return MAP_FAILED;
    }
    if (new_size < old_size) {
      guard.unlock();
      munmap(reinterpret_cast<void*>(old_begin + new_size), old_size - new_size);
    }
    return old_address;
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
  void* destination = MapAnonymous(s, fixed_address, new_size, PROT_READ | PROT_WRITE,
                                   fixed_address != nullptr ? MAP_FIXED : 0);
  if (destination == MAP_FAILED) return MAP_FAILED;
  if (fixed_address != nullptr && destination != fixed_address) {
    errno = EINVAL;
    return MAP_FAILED;
  }

  // Copy, run by run of equal protection; unreadable runs are read through a
  // temporary protection, then get it back.
  size_t copy_size = std::min(old_size, new_size);
  uint8_t* from = reinterpret_cast<uint8_t*>(old_begin);
  uint8_t* to = static_cast<uint8_t*>(destination);
  for (size_t done = 0; done < copy_size;) {
    MEMORY_BASIC_INFORMATION info;
    if (VirtualQuery(from + done, &info, sizeof(info)) == 0) break;
    size_t run = std::min(copy_size - done,
                          static_cast<size_t>(static_cast<uint8_t*>(info.BaseAddress) +
                                              info.RegionSize - (from + done)));
    DWORD protection = info.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    DWORD old_protection;
    bool readable = protection != PAGE_NOACCESS;
    if (!readable) VirtualProtect(from + done, run, PAGE_READONLY, &old_protection);
    memcpy(to + done, from + done, run);
    if (!readable) VirtualProtect(from + done, run, old_protection, &old_protection);
    VirtualProtect(to + done, run, protection, &old_protection);
    done += run;
  }

  if (dont_unmap) {
    // The source stays mapped, as zero pages with its protections.
    for (size_t done = 0; done < old_size;) {
      MEMORY_BASIC_INFORMATION info;
      if (VirtualQuery(from + done, &info, sizeof(info)) == 0) break;
      size_t run = std::min(old_size - done,
                            static_cast<size_t>(static_cast<uint8_t*>(info.BaseAddress) +
                                                info.RegionSize - (from + done)));
      VirtualFree(from + done, run, MEM_DECOMMIT);
      VirtualAlloc(from + done, run, MEM_COMMIT, info.Protect);
      done += run;
    }
  } else {
    guard.unlock();
    munmap(old_address, old_size);
  }
  return destination;
}

}  // extern "C"
