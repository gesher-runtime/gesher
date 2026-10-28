// CI-only data evidence for the real FILE lock. This does not replace WCT.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace native3_crt_lock_probe {
struct Snapshot {
  uintptr_t address = 0, owner = 0, debug = 0;
  LONG recursion = 0, lock_count = 0;
};

class Layout {
 public:
  ~Layout() { if (module_) FreeLibrary(module_); }
  Layout() = default;
  Layout(const Layout&) = delete;
  Layout& operator=(const Layout&) = delete;

  bool Initialize() {
    static_assert(sizeof(void*) == 8 && sizeof(CRITICAL_SECTION) == 40);
    static_assert(offsetof(CRITICAL_SECTION, OwningThread) == 16 &&
                  offsetof(CRITICAL_SECTION, RecursionCount) == 12);
    if (!GetModuleHandleExW(0, L"ucrtbase.dll", &module_)) return false;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return false;
    unsigned entering_offset = 0, leaving_offset = 0;
    const auto enter = GetProcAddress(kernel, "EnterCriticalSection");
    const auto leave = GetProcAddress(kernel, "LeaveCriticalSection");
    const auto lock = GetProcAddress(module_, "_lock_file");
    const auto unlock = GetProcAddress(module_, "_unlock_file");
    // Verify that the public functions used by this actual worker are the
    // inspected named CRT exports, not an unrelated module or fixture lock.
    if (!enter || !leave || !lock || !unlock ||
        reinterpret_cast<uintptr_t>(&_lock_file) != reinterpret_cast<uintptr_t>(lock) ||
        reinterpret_cast<uintptr_t>(&_unlock_file) != reinterpret_cast<uintptr_t>(unlock) ||
        !Decode(lock, enter, &entering_offset) || !Decode(unlock, leave, &leaving_offset) ||
        entering_offset != leaving_offset) return false;
    offset_ = entering_offset;
    char path[32768]{};
    DWORD length = GetModuleFileNameA(module_, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) return false;
    std::fprintf(stderr, "ACTUAL_CRT_LOCK_LAYOUT module=%s base=%p lock=%p unlock=%p enter=%p leave=%p offset=%u decoded=true\n",
        path, static_cast<void*>(module_), reinterpret_cast<void*>(lock), reinterpret_cast<void*>(unlock),
        reinterpret_cast<void*>(enter), reinterpret_cast<void*>(leave), offset_);
    return true;
  }

  bool Read(FILE* stream, Snapshot* output) const {
    uintptr_t start = reinterpret_cast<uintptr_t>(stream);
    if (offset_ == 0 || start > UINTPTR_MAX - offset_ - sizeof(CRITICAL_SECTION)) return false;
    uintptr_t address = start + offset_;
    if (address % alignof(CRITICAL_SECTION) != 0) return false;
    MEMORY_BASIC_INFORMATION stream_region{}, lock_region{};
    if (VirtualQuery(stream, &stream_region, sizeof(stream_region)) != sizeof(stream_region) ||
        VirtualQuery(reinterpret_cast<const void*>(address), &lock_region, sizeof(lock_region)) != sizeof(lock_region) ||
        stream_region.AllocationBase != lock_region.AllocationBase ||
        lock_region.State != MEM_COMMIT || (lock_region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    uintptr_t region = reinterpret_cast<uintptr_t>(lock_region.BaseAddress);
    if (region > address || lock_region.RegionSize < sizeof(CRITICAL_SECTION) ||
        address - region > lock_region.RegionSize - sizeof(CRITICAL_SECTION)) return false;
    CRITICAL_SECTION observed{};
    if (!Copy(reinterpret_cast<const void*>(address), &observed, sizeof(observed))) return false;
    *output = {address, reinterpret_cast<uintptr_t>(observed.OwningThread),
               reinterpret_cast<uintptr_t>(observed.DebugInfo), observed.RecursionCount, observed.LockCount};
    return true;
  }

 private:
  static bool Copy(const void* from, void* to, size_t bytes) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, bytes, &copied) && copied == bytes;
  }

  bool Decode(FARPROC function, FARPROC expected, unsigned* offset) const {
    // Actual two-host implementation40686447043: add rcx,imm8 then tail
    // jmp [rip+disp32]. Unsupported code must fail, never guess a FILE ABI.
    unsigned char code[11]{};
    uintptr_t address = reinterpret_cast<uintptr_t>(function);
    if (!Copy(reinterpret_cast<const void*>(address), code, sizeof(code)) ||
        code[0] != 0x48 || code[1] != 0x83 || code[2] != 0xc1 ||
        code[4] != 0x48 || code[5] != 0xff || code[6] != 0x25 ||
        code[3] < sizeof(FILE) || code[3] >= 0x80) return false;
    int32_t displacement = 0;
    std::memcpy(&displacement, code + 7, sizeof(displacement));
    if (address > UINTPTR_MAX - sizeof(code)) return false;
    uintptr_t following = address + sizeof(code);
    uintptr_t slot = 0;
    if (displacement < 0) {
      uint64_t subtract = static_cast<uint64_t>(-static_cast<int64_t>(displacement));
      if (following < subtract) return false;
      slot = following - subtract;
    } else {
      if (following > UINTPTR_MAX - static_cast<unsigned>(displacement)) return false;
      slot = following + static_cast<unsigned>(displacement);
    }
    MEMORY_BASIC_INFORMATION code_region{}, slot_region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &code_region, sizeof(code_region)) ||
        !VirtualQuery(reinterpret_cast<const void*>(slot), &slot_region, sizeof(slot_region)) ||
        code_region.AllocationBase != module_ || slot_region.AllocationBase != module_ ||
        code_region.Type != MEM_IMAGE || slot_region.Type != MEM_IMAGE) return false;
    uintptr_t target = 0;
    if (!Copy(reinterpret_cast<const void*>(slot), &target, sizeof(target)) ||
        target != reinterpret_cast<uintptr_t>(expected)) return false;
    std::fprintf(stderr, "ACTUAL_CRT_LOCK_CODE function=%p iat=%p target=%p bytes=",
        reinterpret_cast<void*>(function), reinterpret_cast<void*>(slot), reinterpret_cast<void*>(target));
    for (unsigned char byte : code) std::fprintf(stderr, "%02x", static_cast<unsigned>(byte));
    std::fprintf(stderr, "\n");
    *offset = code[3];
    return true;
  }

  HMODULE module_ = nullptr;
  unsigned offset_ = 0;
};

inline void Report(const char* phase, FILE* stream, const Snapshot& value, DWORD owner, uint64_t owner_birth) {
  std::fprintf(stderr, "ACTUAL_CRT_FILE_OWNER phase=%s FILE=%p cs=%p owner=%llu expected-owner=%lu owner-birth=%llu recursion=%ld lock-count=%ld debug=%p\n",
      phase, static_cast<void*>(stream), reinterpret_cast<void*>(value.address),
      static_cast<unsigned long long>(value.owner), owner, static_cast<unsigned long long>(owner_birth),
      value.recursion, value.lock_count, reinterpret_cast<void*>(value.debug));
}
}  // namespace native3_crt_lock_probe
