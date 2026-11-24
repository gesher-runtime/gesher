// Explicit POSIX virtual-memory and profiling-handle boundaries for the ARM64
// TLS component. Production TLS/Env methods are unchanged. This is not Win32
// commitment, a JIT, an Android thread, or execution of translated instructions.
#pragma once

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

using DWORD = uint32_t;
using BOOL = int;
using HANDLE = void*;
constexpr DWORD MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000,
                MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000;
constexpr DWORD PAGE_NOACCESS = 1, PAGE_READWRITE = 4, DUPLICATE_SAME_ACCESS = 2;
constexpr BOOL FALSE = 0;
struct SYSTEM_INFO { DWORD dwPageSize; };
struct MEMORY_BASIC_INFORMATION {
  void* BaseAddress;
  void* AllocationBase;
  size_t RegionSize;
  DWORD State;
  DWORD Protect;
};
namespace vm_boundary {
inline void Require(bool value) {
  if (!value) { std::fputs("invalid TLS VM boundary operation\n", stderr); std::abort(); }
}
inline const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
struct Region { size_t size; std::vector<DWORD> pages; };
inline std::mutex mutex;
inline std::map<uint8_t*, Region> regions;
inline std::atomic<DWORD> next_thread{1};
inline thread_local DWORD thread = next_thread.fetch_add(1);
inline auto RegionAt(uint8_t* at, size_t bytes) {
  auto found = regions.upper_bound(at);
  if (found == regions.begin()) return regions.end();
  --found;
  const uintptr_t offset = reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(found->first);
  if (offset >= found->second.size || bytes > found->second.size - offset) return regions.end();
  return found;
}
}  // namespace vm_boundary
inline void GetSystemInfo(SYSTEM_INFO* info) { info->dwPageSize = vm_boundary::page; }
inline DWORD GetCurrentThreadId() { return vm_boundary::thread; }
inline DWORD GetLastError() { return 0; }
inline HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(uintptr_t{1}); }
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(uintptr_t{2}); }
inline BOOL DuplicateHandle(HANDLE process, HANDLE thread, HANDLE target, HANDLE* result,
                            DWORD access, BOOL inherit, DWORD options) {
  vm_boundary::Require(process == GetCurrentProcess() && target == process &&
                       thread == GetCurrentThread() && access == 0 && !inherit &&
                       options == DUPLICATE_SAME_ACCESS);
  *result = new unsigned(0x544c53);
  return 1;
}
inline BOOL CloseHandle(HANDLE value) {
  auto* handle = static_cast<unsigned*>(value);
  vm_boundary::Require(handle && *handle == 0x544c53);
  delete handle;
  return 1;
}
inline void* VirtualAlloc(void* address, size_t bytes, DWORD flags, DWORD protection) {
  using namespace vm_boundary;
  Require(bytes != 0 && bytes % page == 0 && protection == PAGE_READWRITE);
  std::lock_guard<std::mutex> lock(mutex);
  if (flags == (MEM_RESERVE | MEM_COMMIT)) {
    Require(address == nullptr);
    void* allocated = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocated == MAP_FAILED) return nullptr;
    regions.emplace(static_cast<uint8_t*>(allocated), Region{bytes, std::vector<DWORD>(bytes / page, protection)});
    return allocated;
  }
  Require(flags == MEM_COMMIT && reinterpret_cast<uintptr_t>(address) % page == 0);
  auto* at = static_cast<uint8_t*>(address);
  auto found = RegionAt(at, bytes);
  if (found == regions.end()) return nullptr;
  if (mprotect(address, bytes, PROT_READ | PROT_WRITE) != 0) return nullptr;
  const size_t first = (at - found->first) / page;
  std::fill_n(found->second.pages.begin() + first, bytes / page, protection);
  return address;
}
inline BOOL VirtualFree(void* address, size_t bytes, DWORD flags) {
  using namespace vm_boundary;
  std::lock_guard<std::mutex> lock(mutex);
  auto* at = static_cast<uint8_t*>(address);
  if (flags == MEM_RELEASE) {
    Require(bytes == 0);
    auto found = regions.find(at);
    Require(found != regions.end());
    Require(munmap(address, found->second.size) == 0);
    regions.erase(found);
    return 1;
  }
  Require(flags == MEM_DECOMMIT && bytes != 0 && bytes % page == 0 &&
          reinterpret_cast<uintptr_t>(address) % page == 0);
  auto found = RegionAt(at, bytes);
  Require(found != regions.end());
  Require(mprotect(address, bytes, PROT_NONE) == 0);
  Require(madvise(address, bytes, MADV_DONTNEED) == 0);
  const size_t first = (at - found->first) / page;
  std::fill_n(found->second.pages.begin() + first, bytes / page, DWORD{0});
  return 1;
}
inline BOOL VirtualProtect(void* address, size_t bytes, DWORD protection, DWORD* old) {
  using namespace vm_boundary;
  Require(protection == PAGE_NOACCESS && bytes % page == 0);
  std::lock_guard<std::mutex> lock(mutex);
  auto* at = static_cast<uint8_t*>(address);
  auto found = RegionAt(at, bytes);
  Require(found != regions.end());
  const size_t first = (at - found->first) / page;
  *old = found->second.pages[first];
  Require(mprotect(address, bytes, PROT_NONE) == 0);
  std::fill_n(found->second.pages.begin() + first, bytes / page, protection);
  return 1;
}
inline size_t VirtualQuery(void* address, MEMORY_BASIC_INFORMATION* info, size_t bytes) {
  using namespace vm_boundary;
  Require(bytes == sizeof(*info));
  std::lock_guard<std::mutex> lock(mutex);
  auto* at = static_cast<uint8_t*>(address);
  auto found = RegionAt(at, 1);
  if (found == regions.end()) return 0;
  size_t first = (at - found->first) / page, end = first + 1;
  DWORD protection = found->second.pages[first];
  while (end < found->second.pages.size() && found->second.pages[end] == protection) ++end;
  *info = {found->first + first * page, found->first, (end - first) * page,
           protection ? MEM_COMMIT : MEM_RESERVE, protection};
  return sizeof(*info);
}
