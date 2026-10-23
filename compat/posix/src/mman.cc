// gesher-base: mmap family on Win32.
//
// Windows frees memory per allocation (VirtualAlloc) or per view
// (MapViewOfFile), while mmap callers unmap and re-protect arbitrary page
// ranges. Every mapping is therefore tracked with a per-page liveness bitmap:
//   - unmapping part of an anonymous region decommits those pages;
//   - unmapping part of a file view makes those pages inaccessible;
//   - when no live page remains, the allocation or view is released.
//
// mremap moves anonymous mappings by copying (see mremap below).
//
// Not supported yet (fails with EINVAL):
//   - MAP_FIXED over memory this layer does not own, or over a file view
//     (needs placeholder mappings: VirtualAlloc2 / MapViewOfFile3);
//   - executable file views (needs a handle opened with GENERIC_EXECUTE).

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

namespace {

struct Region {
  size_t size = 0;            // bytes, multiple of the page size
  bool anonymous = true;
  void* view = nullptr;       // file views: what MapViewOfFile returned
  HANDLE mapping = nullptr;   // file views: the mapping object
  std::vector<bool> live;     // one entry per page
  size_t live_pages = 0;
};

struct State {
  std::mutex lock;
  std::map<uintptr_t, Region> regions;  // keyed by region start
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

// Region containing `addr`, or end().
std::map<uintptr_t, Region>::iterator FindRegion(State& s, uintptr_t addr) {
  auto it = s.regions.upper_bound(addr);
  if (it == s.regions.begin()) return s.regions.end();
  --it;
  return addr < it->first + it->second.size ? it : s.regions.end();
}

void AddRegion(State& s, void* start, size_t size, Region region) {
  region.size = size;
  region.live.assign(size / s.page_size, true);
  region.live_pages = region.live.size();
  s.regions.emplace(reinterpret_cast<uintptr_t>(start), std::move(region));
}

void Release(uintptr_t start, Region& region) {
  if (region.anonymous) {
    VirtualFree(reinterpret_cast<void*>(start), 0, MEM_RELEASE);
  } else {
    UnmapViewOfFile(region.view);
    CloseHandle(region.mapping);
  }
}

void* MapAnonymous(State& s, void* start, size_t len, int prot, int flags) {
  DWORD protection = PageProtection(prot);
  uintptr_t addr = reinterpret_cast<uintptr_t>(start);
  bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;

  // MAP_FIXED inside one of our anonymous regions: replace those pages with
  // fresh zero pages, as mmap does.
  if ((flags & MAP_FIXED) != 0) {
    auto it = FindRegion(s, addr);
    if (it != s.regions.end() && it->second.anonymous &&
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
  AddRegion(s, result, len, Region{});
  return result;
}

void* MapFile(State& s, void* start, size_t len, int prot, int flags, int fd, off_t offset) {
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

  // A view may not extend past the end of the file (mmap rounds the length up
  // to pages; a file such as an ashmem region need not be); length 0 maps up
  // to the end instead (the tail of the last page reads as zeros).
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
  region.anonymous = false;
  region.view = view;
  region.mapping = mapping;
  AddRegion(s, result, RoundUp(len, s.page_size), std::move(region));
  return result;
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
  TRACE("mmap(%p, 0x%zx, prot=%d, flags=0x%x, fd=%d) = %p\n", addr, length, prot, flags, fd,
        result);
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
  auto it = FindRegion(s, begin);
  if (it == s.regions.end()) it = s.regions.lower_bound(begin);
  while (it != s.regions.end() && it->first < end) {
    uintptr_t region_begin = it->first;
    Region& region = it->second;
    uintptr_t from = std::max(begin, region_begin);
    uintptr_t to = std::min(end, region_begin + region.size);
    if (from < to) {
      for (size_t i = (from - region_begin) / s.page_size, n = (to - region_begin) / s.page_size;
           i < n; ++i) {
        if (region.live[i]) {
          region.live[i] = false;
          --region.live_pages;
        }
      }
      if (region.live_pages == 0) {
        Release(region_begin, region);
        it = s.regions.erase(it);
        continue;
      }
      if (region.anonymous) {
        VirtualFree(reinterpret_cast<void*>(from), to - from, MEM_DECOMMIT);
      } else {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(from), to - from, PAGE_NOACCESS, &old);
      }
    }
    ++it;
  }
  return 0;
}

int mprotect(void* addr, size_t length, int prot) {
  TRACE("mprotect(%p, 0x%zx, %d)\n", addr, length, prot);
  if (length == 0) return 0;
  DWORD protection = PageProtection(prot);
  // Copy-on-write views stay copy-on-write when made writable.
  MEMORY_BASIC_INFORMATION info;
  if (protection == PAGE_READWRITE && VirtualQuery(addr, &info, sizeof(info)) != 0 &&
      info.Type == MEM_MAPPED &&
      (info.AllocationProtect == PAGE_WRITECOPY ||
       info.AllocationProtect == PAGE_EXECUTE_WRITECOPY)) {
    protection = PAGE_WRITECOPY;
  }
  DWORD old;
  if (!VirtualProtect(addr, length, protection, &old)) {
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
      MEMORY_BASIC_INFORMATION info;
      if (VirtualQuery(addr, &info, sizeof(info)) != 0 && info.Type == MEM_PRIVATE &&
          info.State == MEM_COMMIT) {
        DWORD protection = info.Protect;
        VirtualFree(addr, length, MEM_DECOMMIT);
        if (VirtualAlloc(addr, length, MEM_COMMIT, protection) == nullptr) {
          errno = ENOMEM;
          return -1;
        }
      } else if (info.Type == MEM_MAPPED && (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
        memset(addr, 0, length);
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
  // The source: live pages of one anonymous region.
  auto source = FindRegion(s, old_begin);
  if (source == s.regions.end() || old_begin + old_size > source->first + source->second.size) {
    errno = EFAULT;
    return MAP_FAILED;
  }
  if (!source->second.anonymous) {
    errno = EINVAL;  // a file view stays where Windows put it
    return MAP_FAILED;
  }
  for (size_t i = (old_begin - source->first) / s.page_size, n = i + old_size / s.page_size; i < n;
       ++i) {
    if (!source->second.live[i]) {
      errno = EFAULT;
      return MAP_FAILED;
    }
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
