/*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright (C) 2026 The Gesher Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Gesher overlay: replaces AOSP's art/libartbase/base/mem_map_windows.cc.
//
// Upstream only supports read/read-write file mappings and never unmaps.
// ART needs mmap semantics: anonymous memory, address hints, MAP_FIXED, and
// unmapping arbitrary page ranges (it trims reservations to alignment and
// splits maps). Windows releases memory per allocation, not per page, so every
// mapping is tracked with a per-page liveness bitmap:
//   - unmapping part of an anonymous region decommits those pages;
//   - unmapping part of a file view makes those pages inaccessible;
//   - when no live page remains, the allocation or view is released.

#include "base/mem_map.h"

#include <windows.h>
// windows.h defines ERROR; see the comment in android-base/logging.h.
#ifdef ERROR
#undef ERROR
#endif

#include <io.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "android-base/logging.h"
#include "android-base/stringprintf.h"
#include "base/bit_utils.h"
#ifdef PROT_READ
#undef PROT_READ
#endif
#ifdef PROT_WRITE
#undef PROT_WRITE
#endif
#include "base/mman.h"

namespace art {

using android::base::StringPrintf;

namespace {

size_t page_size;
size_t allocation_granularity;

struct Region {
  size_t size = 0;                  // bytes, multiple of page_size
  bool anonymous = true;
  void* view = nullptr;             // file views: what MapViewOfFile returned
  HANDLE mapping = nullptr;         // file views: the mapping object
  std::vector<bool> live;           // one entry per page
  size_t live_pages = 0;
};

// Keyed by region start. Never destroyed: mappings outlive static destructors.
std::mutex& RegionsLock() {
  static std::mutex* lock = new std::mutex();
  return *lock;
}
std::map<uintptr_t, Region>& Regions() {
  static auto* regions = new std::map<uintptr_t, Region>();
  return *regions;
}

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

// Returns the region containing `addr`, or end().
std::map<uintptr_t, Region>::iterator FindRegion(uintptr_t addr) {
  auto& regions = Regions();
  auto it = regions.upper_bound(addr);
  if (it == regions.begin()) return regions.end();
  --it;
  return addr < it->first + it->second.size ? it : regions.end();
}

void AddRegion(void* start, size_t size, Region region) {
  region.size = size;
  region.live.assign(size / page_size, true);
  region.live_pages = region.live.size();
  Regions().emplace(reinterpret_cast<uintptr_t>(start), std::move(region));
}

void Release(uintptr_t start, Region& region) {
  if (region.anonymous) {
    if (!::VirtualFree(reinterpret_cast<void*>(start), 0, MEM_RELEASE)) {
      PLOG(ERROR) << StringPrintf("VirtualFree(%p) failed %lx", reinterpret_cast<void*>(start),
                                  ::GetLastError());
    }
  } else {
    ::UnmapViewOfFile(region.view);
    ::CloseHandle(region.mapping);
  }
}

void* WinMapAnonymous(void* start, size_t len, int prot, int flags) {
  DWORD protection = PageProtection(prot);
  uintptr_t addr = reinterpret_cast<uintptr_t>(start);

  // MAP_FIXED inside one of our anonymous regions: replace those pages with
  // fresh zero pages, as mmap does.
  if ((flags & MAP_FIXED) != 0) {
    auto it = FindRegion(addr);
    if (it != Regions().end() && it->second.anonymous &&
        addr + len <= it->first + it->second.size) {
      ::VirtualFree(start, len, MEM_DECOMMIT);
      if (::VirtualAlloc(start, len, MEM_COMMIT, protection) == nullptr) {
        PLOG(ERROR) << StringPrintf("VirtualAlloc(MEM_COMMIT, %p, %zu) failed %lx", start, len,
                                    ::GetLastError());
        errno = ENOMEM;
        return MAP_FAILED;
      }
      Region& region = it->second;
      for (size_t i = (addr - it->first) / page_size, n = i + len / page_size; i < n; ++i) {
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
    result = ::VirtualAlloc(start, len, MEM_RESERVE | MEM_COMMIT, protection);
    if (result == nullptr && (flags & MAP_FIXED) != 0) {
      // TODO(gesher): MAP_FIXED over a region owned by something else needs
      // placeholder mappings (VirtualAlloc2 / MapViewOfFile3).
      LOG(ERROR) << StringPrintf("MAP_FIXED anonymous map at %p unavailable (%lx)", start,
                                 ::GetLastError());
      errno = EINVAL;
      return MAP_FAILED;
    }
  }
  if (result == nullptr) {
    result = ::VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, protection);
  }
  if (result == nullptr) {
    PLOG(ERROR) << StringPrintf("VirtualAlloc(%zu) failed %lx", len, ::GetLastError());
    errno = ENOMEM;
    return MAP_FAILED;
  }
  AddRegion(result, len, Region{});
  return result;
}

void* WinMapFile(void* start, size_t len, int prot, int flags, int fd, off_t fd_off) {
  if ((prot & PROT_EXEC) != 0) {
    // TODO(gesher): executable file views need a handle opened with
    // GENERIC_EXECUTE; needed for .oat files in the runtime port.
    LOG(ERROR) << "Executable file mappings are not supported yet.";
    errno = EINVAL;
    return MAP_FAILED;
  }

  size_t padding = static_cast<size_t>(fd_off) % allocation_granularity;
  off_t file_offset = fd_off - static_cast<off_t>(padding);
  size_t map_length = len + padding;

  DWORD map_access;
  DWORD view_access;
  if ((prot & PROT_WRITE) != 0) {
    if ((flags & MAP_SHARED) != 0 && (flags & MAP_PRIVATE) == 0) {
      map_access = PAGE_READWRITE;
      view_access = FILE_MAP_ALL_ACCESS;
    } else if ((flags & MAP_SHARED) == 0 && (flags & MAP_PRIVATE) != 0) {
      map_access = PAGE_WRITECOPY;
      view_access = FILE_MAP_COPY;
    } else {
      LOG(ERROR) << "MAP_PRIVATE and MAP_SHARED inconsistently set.";
      errno = EINVAL;
      return MAP_FAILED;
    }
  } else {
    map_access = PAGE_READONLY;
    view_access = FILE_MAP_READ;
  }

  HANDLE file_handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (file_handle == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "Bad file descriptor " << fd;
    errno = EBADF;
    return MAP_FAILED;
  }
  // A read-only view may not extend past the end of the file; a length of zero
  // maps up to the end instead.
  LARGE_INTEGER file_length;
  if (!::GetFileSizeEx(file_handle, &file_length)) {
    PLOG(ERROR) << StringPrintf("Couldn't get file size %lx.", ::GetLastError());
    errno = EINVAL;
    return MAP_FAILED;
  }
  size_t view_length = map_length;
  if (map_access == PAGE_READONLY &&
      file_offset + static_cast<int64_t>(map_length) > file_length.QuadPart) {
    view_length = 0;
  }

  HANDLE mapping = ::CreateFileMappingW(file_handle, nullptr, map_access, 0, 0, nullptr);
  if (mapping == nullptr) {
    PLOG(ERROR) << StringPrintf("Couldn't create file mapping %lx.", ::GetLastError());
    errno = EINVAL;
    return MAP_FAILED;
  }

  DWORD offset_low = static_cast<DWORD>(static_cast<uint64_t>(file_offset) & 0xffffffffU);
  DWORD offset_high = static_cast<DWORD>(static_cast<uint64_t>(file_offset) >> 32);
  void* want = start == nullptr
      ? nullptr
      : reinterpret_cast<uint8_t*>(start) - padding;
  void* view = ::MapViewOfFileEx(mapping, view_access, offset_high, offset_low, view_length, want);
  if (view == nullptr && want != nullptr && (flags & MAP_FIXED) == 0) {
    view = ::MapViewOfFileEx(mapping, view_access, offset_high, offset_low, view_length, nullptr);
  }
  if (view == nullptr) {
    // TODO(gesher): MAP_FIXED over an existing reservation needs placeholders.
    PLOG(ERROR) << StringPrintf("Couldn't create file view %lx.", ::GetLastError());
    ::CloseHandle(mapping);
    errno = EINVAL;
    return MAP_FAILED;
  }

  void* result = reinterpret_cast<uint8_t*>(view) + padding;
  Region region;
  region.anonymous = false;
  region.view = view;
  region.mapping = mapping;
  AddRegion(result, RoundUp(len, page_size), std::move(region));
  return result;
}

}  // namespace

void MemMap::TargetMMapInit() {
  SYSTEM_INFO si;
  ::GetSystemInfo(&si);
  page_size = si.dwPageSize;
  allocation_granularity = si.dwAllocationGranularity;
}

void* MemMap::TargetMMap(void* start, size_t len, int prot, int flags, int fd, off_t fd_off) {
  if (len == 0 || len % page_size != 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  std::lock_guard<std::mutex> guard(RegionsLock());
  if ((flags & MAP_ANONYMOUS) != 0 || fd == -1) {
    return WinMapAnonymous(start, len, prot, flags);
  }
  return WinMapFile(start, len, prot, flags, fd, fd_off);
}

int MemMap::TargetMUnmap(void* start, size_t len) {
  uintptr_t begin = reinterpret_cast<uintptr_t>(start);
  if (begin % page_size != 0) {
    errno = EINVAL;
    return -1;
  }
  uintptr_t end = begin + RoundUp(len, page_size);

  std::lock_guard<std::mutex> guard(RegionsLock());
  auto& regions = Regions();
  auto it = FindRegion(begin);
  if (it == regions.end()) it = regions.lower_bound(begin);
  while (it != regions.end() && it->first < end) {
    uintptr_t region_begin = it->first;
    Region& region = it->second;
    uintptr_t from = std::max(begin, region_begin);
    uintptr_t to = std::min(end, region_begin + region.size);
    if (from < to) {
      for (size_t i = (from - region_begin) / page_size, n = (to - region_begin) / page_size;
           i < n; ++i) {
        if (region.live[i]) {
          region.live[i] = false;
          --region.live_pages;
        }
      }
      if (region.live_pages == 0) {
        Release(region_begin, region);
        it = regions.erase(it);
        continue;
      }
      void* range = reinterpret_cast<void*>(from);
      if (region.anonymous) {
        ::VirtualFree(range, to - from, MEM_DECOMMIT);
      } else {
        DWORD old;
        ::VirtualProtect(range, to - from, PAGE_NOACCESS, &old);
      }
    }
    ++it;
  }
  return 0;
}

}  // namespace art

// Declared in base/mman.h (Gesher patch). Copy-on-write file views keep
// copy-on-write semantics when made writable again.
int mprotect(void* addr, size_t len, int prot) {
  if (len == 0) return 0;
  DWORD protection = art::PageProtection(prot);
  MEMORY_BASIC_INFORMATION info;
  if (protection == PAGE_READWRITE && ::VirtualQuery(addr, &info, sizeof(info)) != 0 &&
      info.Type == MEM_MAPPED &&
      (info.AllocationProtect == PAGE_WRITECOPY ||
       info.AllocationProtect == PAGE_EXECUTE_WRITECOPY)) {
    protection = PAGE_WRITECOPY;
  }
  DWORD old;
  if (!::VirtualProtect(addr, len, protection, &old)) {
    errno = EACCES;
    return -1;
  }
  return 0;
}
