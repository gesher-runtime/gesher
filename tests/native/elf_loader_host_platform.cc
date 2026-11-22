// Linux primitives for the explicitly isolated full-production ELF loader test.
#include <windows.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace {
constexpr size_t kPage = 4096;
size_t Rounded(size_t n) { return (n + kPage - 1) & ~(kPage - 1); }
std::mutex platform_mutex;
std::map<uintptr_t, size_t> reservations;
std::array<FlsCallback, 64> fls_callbacks{};
std::atomic<DWORD> next_thread{1}, next_tls{0}, next_fls{0};
std::atomic<bool> fail_next_protection{false};
struct ThreadState {
  DWORD id = next_thread.fetch_add(1);
  std::array<void*, 64> tls{}, fls{};
  NT_TIB tib;
  ~ThreadState() {
    for (size_t i = 0; i < fls.size(); ++i) {
      if (fls[i] != nullptr && fls_callbacks[i] != nullptr) {
        void* p = fls[i]; fls[i] = nullptr; fls_callbacks[i](p);
      }
    }
  }
};
thread_local ThreadState state;
int Protection(DWORD p) {
  switch (p & 0xff) {
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: case PAGE_WRITECOPY: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
  }
}
}

DWORD GetCurrentThreadId() { return state.id; }
unsigned long GetCurrentProcessId() { return static_cast<unsigned long>(getpid()); }
DWORD TlsAlloc() { return next_tls.fetch_add(1); }
BOOL TlsFree(DWORD) { return TRUE; }
void* TlsGetValue(DWORD i) { return state.tls.at(i); }
BOOL TlsSetValue(DWORD i, void* p) { state.tls.at(i) = p; return TRUE; }
DWORD FlsAlloc(FlsCallback callback) {
  DWORD i = next_fls.fetch_add(1); fls_callbacks.at(i) = callback; return i;
}
void* FlsGetValue(DWORD i) { return state.fls.at(i); }
BOOL FlsSetValue(DWORD i, void* p) { state.fls.at(i) = p; return TRUE; }
BOOL InitOnceExecuteOnce(PINIT_ONCE once, InitCallback callback, PVOID p, PVOID* context) {
  std::call_once(once->once, [&] { if (!callback(once, p, context)) std::abort(); });
  return TRUE;
}
void* VirtualAlloc(void* address, size_t bytes, DWORD type, DWORD protection) {
  if (bytes == 0) return nullptr;
  const size_t size = Rounded(bytes);
  std::lock_guard<std::mutex> lock(platform_mutex);
  if (type & MEM_RESERVE) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (address != nullptr) flags |= MAP_FIXED_NOREPLACE;
    void* p = mmap(address, size, (type & MEM_COMMIT) ? Protection(protection) : PROT_NONE,
                   flags, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    reservations.emplace(reinterpret_cast<uintptr_t>(p), size);
    return p;
  }
  if (type != MEM_COMMIT) return nullptr;
  uintptr_t start = reinterpret_cast<uintptr_t>(address);
  auto it = reservations.upper_bound(start);
  if (it == reservations.begin()) return nullptr;
  --it;
  if (start < it->first || start + size > it->first + it->second) return nullptr;
  return mprotect(address, size, Protection(protection)) == 0 ? address : nullptr;
}
BOOL VirtualFree(void* address, size_t size, DWORD type) {
  std::lock_guard<std::mutex> lock(platform_mutex);
  auto it = reservations.find(reinterpret_cast<uintptr_t>(address));
  if (type == MEM_RELEASE) {
    if (it == reservations.end() || size != 0) return FALSE;
    if (munmap(address, it->second) != 0) return FALSE;
    reservations.erase(it); return TRUE;
  }
  if (type != MEM_DECOMMIT || size == 0) return FALSE;
  return mprotect(address, Rounded(size), PROT_NONE) == 0 &&
         madvise(address, Rounded(size), MADV_DONTNEED) == 0;
}
size_t VirtualQuery(void* address, MEMORY_BASIC_INFORMATION* info, size_t) {
  const uintptr_t at = reinterpret_cast<uintptr_t>(address);
  FILE* maps = std::fopen("/proc/self/maps", "r");
  if (maps == nullptr) return 0;
  char line[1024], access[5];
  unsigned long long start, end;
  uintptr_t previous = 0;
  bool found = false;
  while (std::fgets(line, sizeof(line), maps)) {
    if (std::sscanf(line, "%llx-%llx %4s", &start, &end, access) != 3) continue;
    if (at < start) {
      info->BaseAddress = reinterpret_cast<void*>(previous);
      info->RegionSize = start - previous; info->State = MEM_FREE; info->Protect = PAGE_NOACCESS;
      found = true; break;
    }
    if (at < end) {
      info->BaseAddress = reinterpret_cast<void*>(static_cast<uintptr_t>(start));
      info->RegionSize = end - start; info->State = MEM_COMMIT;
      info->Protect = access[2] == 'x' ? (access[1] == 'w' ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
          : access[1] == 'w' ? PAGE_READWRITE : access[0] == 'r' ? PAGE_READONLY : PAGE_NOACCESS;
      info->Type = MEM_PRIVATE; found = true; break;
    }
    previous = end;
  }
  std::fclose(maps);
  return found ? sizeof(*info) : 0;
}
BOOL VirtualProtect(void* address, size_t size, DWORD p, DWORD* old) {
  if (fail_next_protection.exchange(false)) return FALSE;
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(address, &info, sizeof(info))) return FALSE;
  if (old) *old = info.Protect;
  return mprotect(address, Rounded(size), Protection(p)) == 0;
}
void GetCurrentThreadStackLimits(ULONG_PTR* low, ULONG_PTR* high) { *low = *high = 0; }
void* NtCurrentTeb() { return &state.tib; }
void* _aligned_malloc(size_t size, size_t align) {
  void* p = nullptr; return posix_memalign(&p, align, size) == 0 ? p : nullptr;
}
void _aligned_free(void* p) { std::free(p); }
void AcquireSRWLockExclusive(SRWLOCK* lock) { lock->lock(); }
void ReleaseSRWLockExclusive(SRWLOCK* lock) { lock->unlock(); }
void AcquireSRWLockShared(SRWLOCK* lock) { lock->lock_shared(); }
void ReleaseSRWLockShared(SRWLOCK* lock) { lock->unlock_shared(); }
LONG InterlockedIncrement(volatile LONG* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
PVOID AddVectoredExceptionHandler(ULONG_PTR, LONG (*)(EXCEPTION_POINTERS*)) {
  static int registration; return &registration;  // no Windows fault dispatch on this host
}
HANDLE GetCurrentProcess() { return nullptr; }
BOOL FlushInstructionCache(HANDLE, const void* address, size_t size) {
  auto* p = const_cast<char*>(static_cast<const char*>(address));
  __builtin___clear_cache(p, p + size); return TRUE;
}
BOOL ReadProcessMemory(HANDLE, const void* source, void* target, size_t size, SIZE_T* copied) {
  // Only own fixture mappings are read, with no concurrent mapping mutation.
  *copied = 0;
  const uintptr_t first = reinterpret_cast<uintptr_t>(source);
  if (size > UINTPTR_MAX - first) return FALSE;
  for (uintptr_t at = first; at < first + size;) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || info.Protect == PAGE_NOACCESS) return FALSE;
    uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (end <= at) return FALSE;
    at = std::min(end, first + size);
  }
  std::memcpy(target, source, size); *copied = size; return TRUE;
}
HANDLE CreateFileMappingW(HANDLE, void*, DWORD, DWORD, DWORD, const wchar_t*) { return nullptr; }
void* MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T) { return nullptr; }
BOOL UnmapViewOfFile(void*) { return FALSE; }
BOOL CloseHandle(HANDLE) { return TRUE; }
BOOL FlushViewOfFile(const void*, SIZE_T) { return FALSE; }
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*) { return FALSE; }
void GetSystemInfo(SYSTEM_INFO* info) { info->dwAllocationGranularity = 65536; }
extern "C" int gesher_mmap_placeholders() { return 0; }
extern "C" int gesher_mmap_replaced_private_pages() { return 0; }
namespace elf_host_platform {
void FailNextVirtualProtect() { fail_next_protection = true; }
size_t LiveReservations() { std::lock_guard<std::mutex> lock(platform_mutex); return reservations.size(); }
size_t ReservedBytes() {
  std::lock_guard<std::mutex> lock(platform_mutex);
  size_t sum = 0; for (const auto& r : reservations) sum += r.second; return sum;
}
}
