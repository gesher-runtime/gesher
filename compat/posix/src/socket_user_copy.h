// Shared by the production Winsock bridge and the actual Windows copy probe.
#pragma once
#include <windows.h>
#include <stdint.h>

namespace gesher::ws::usercopy {
inline bool From(void* destination, const void* source, size_t size) {
  if (size == 0) return true;
  uintptr_t at = reinterpret_cast<uintptr_t>(source);
  if (size > UINTPTR_MAX - at) return false;
  const uintptr_t end = at + size;
  while (at < end) {
    MEMORY_BASIC_INFORMATION region = {};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) != 0) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (base > at || region.RegionSize > UINTPTR_MAX - base) return false;
    uintptr_t next = base + region.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  SIZE_T copied = 0;
  return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) && copied == size;
}
inline bool To(void* destination, const void* source, size_t size) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  uintptr_t at = reinterpret_cast<uintptr_t>(destination);
  if (size > UINTPTR_MAX - at) return false;
  const auto* input = static_cast<const unsigned char*>(source);
  while (size != 0) {
    MEMORY_BASIC_INFORMATION region = {};
    if (VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) != 0) return false;
    DWORD protection = region.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    // RPM's reported partial byte count does not prove destination writes.
    // One page per call preserves Linux's measured valid-page prefix before
    // a later inaccessible page. VirtualQuery is only a protection prefilter:
    // the kernel copy still handles unmap/protect races without a shim fault.
    size_t part = info.dwPageSize - (at % info.dwPageSize);
    if (part > size) part = size;
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), input, reinterpret_cast<void*>(at), part, &copied) ||
        copied != part) return false;
    at += part; input += part; size -= part;
  }
  return true;
}
}  // namespace gesher::ws::usercopy
