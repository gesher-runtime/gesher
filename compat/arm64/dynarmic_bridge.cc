// gesher: native direct execution bridge for Windows on ARM64.
// When running on native AArch64 Windows host hardware, ARM64 guest code can
// execute directly without Dynarmic CPU translation.

#include <gesher/arm64_translator.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace gesher::arm64 {

#if defined(__aarch64__)

namespace {

struct CodeRegion {
  uintptr_t start;
  uintptr_t end;
};

std::shared_mutex g_regions_mutex;
std::vector<CodeRegion> g_regions;

void NativeAddCode(const void* start, size_t size, const char* /*name*/) {
  uintptr_t s = reinterpret_cast<uintptr_t>(start);
  uintptr_t e = s + size;
  {
    std::unique_lock<std::shared_mutex> lock(g_regions_mutex);
    g_regions.push_back({s, e});
  }
  FlushInstructionCache(GetCurrentProcess(), start, size);
}

uint64_t NativeCall(const void* fn, const uint64_t* args, size_t nargs) {
  uint64_t a0 = nargs > 0 ? args[0] : 0;
  uint64_t a1 = nargs > 1 ? args[1] : 0;
  uint64_t a2 = nargs > 2 ? args[2] : 0;
  uint64_t a3 = nargs > 3 ? args[3] : 0;
  uint64_t a4 = nargs > 4 ? args[4] : 0;
  uint64_t a5 = nargs > 5 ? args[5] : 0;
  uint64_t a6 = nargs > 6 ? args[6] : 0;
  uint64_t a7 = nargs > 7 ? args[7] : 0;
  auto target = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t, uint64_t)>(
      reinterpret_cast<uintptr_t>(fn));
  return target(a0, a1, a2, a3, a4, a5, a6, a7);
}

void* NativeHostEntry(const void* fn) {
  return const_cast<void*>(fn);
}

void* NativeBind(void* host_fn) {
  return host_fn;
}

void* NativeGuestSymbol(const char* /*name*/) {
  return nullptr;
}

void NativeAddVaListFunction(const void* /*fn*/, int /*index*/) {}

constexpr size_t kTcbSize = 64;
constexpr size_t kTlsBlockSize = 256 * 1024;
std::mutex g_tls_mutex;
size_t g_tls_offset = kTcbSize;

struct TlsImage {
  int64_t offset;
  const void* image;
  size_t image_size;
  size_t total_size;
};

std::vector<TlsImage> g_tls_images;
std::vector<uint8_t*> g_tls_blocks;

int64_t NativeTlsRegister(size_t total_size, size_t align) {
  std::lock_guard<std::mutex> lock(g_tls_mutex);
  if (align < 1) align = 1;
  size_t off = (g_tls_offset + align - 1) & ~(align - 1);
  if (off + total_size > kTlsBlockSize) return -1;
  g_tls_offset = off + total_size;
  return static_cast<int64_t>(off);
}

void NativeTlsPublish(int64_t offset, const void* image, size_t image_size, size_t total_size) {
  std::lock_guard<std::mutex> lock(g_tls_mutex);
  g_tls_images.push_back({offset, image, image_size, total_size});
  for (uint8_t* base : g_tls_blocks) {
    std::memset(base + offset, 0, total_size);
    if (image != nullptr && image_size > 0) {
      std::memcpy(base + offset, image, image_size);
    }
  }
}

void* g_tlsdesc_static = nullptr;

void* NativeTlsDescStatic(void) {
  return g_tlsdesc_static;
}

int NativeIsGuestCode(const void* addr) {
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  std::shared_lock<std::shared_mutex> lock(g_regions_mutex);
  for (const auto& r : g_regions) {
    if (a >= r.start && a < r.end) return 1;
  }
  return 0;
}

extern "C" uint64_t GesherRunAarch64Program(const void* entry, const uint64_t* block, size_t words);

__asm__(
    ".text\n"
    ".globl GesherRunAarch64Program\n"
    ".p2align 2\n"
    "GesherRunAarch64Program:\n"
    "  stp x29, x30, [sp, #-32]!\n"
    "  mov x29, sp\n"
    "  str x19, [sp, #16]\n"
    "  mov x19, x0\n"
    "  lsl x3, x2, #3\n"
    "  add x3, x3, #15\n"
    "  bic x3, x3, #15\n"
    "  sub sp, sp, x3\n"
    "  mov x4, sp\n"
    "  cbz x2, 2f\n"
    "1:\n"
    "  ldr x5, [x1], #8\n"
    "  str x5, [x4], #8\n"
    "  sub x2, x2, #1\n"
    "  cbnz x2, 1b\n"
    "2:\n"
    "  mov x0, #0\n"
    "  blr x19\n"
    "  mov sp, x29\n"
    "  ldr x19, [sp, #16]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  ret\n"
);

uint64_t NativeRunProgram(const void* entry, const uint64_t* block, size_t words) {
  return GesherRunAarch64Program(entry, block, words);
}

gesher_arm64_api g_native_api = {
    GESHER_ARM64_API_VERSION,
    NativeAddCode,
    NativeCall,
    NativeHostEntry,
    NativeGuestSymbol,
    NativeAddVaListFunction,
    NativeTlsRegister,
    NativeTlsPublish,
    NativeTlsDescStatic,
    NativeIsGuestCode,
    NativeRunProgram,
};

}  // namespace

bool InitDynarmicBridge(const gesher_arm64_host* host, const gesher_arm64_api** out_api,
                        void* (**out_bind)(void*)) {
  if (host == nullptr || host->version != GESHER_ARM64_API_VERSION) {
    return false;
  }
  if (g_tlsdesc_static == nullptr) {
    // 0xF9400400: ldr x0, [x0, #8]
    // 0xD65F03C0: ret
    static const uint32_t tlsdesc_code[2] = {0xF9400400u, 0xD65F03C0u};
    void* mem = VirtualAlloc(nullptr, sizeof(tlsdesc_code), MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (mem != nullptr) {
      std::memcpy(mem, tlsdesc_code, sizeof(tlsdesc_code));
      FlushInstructionCache(GetCurrentProcess(), mem, sizeof(tlsdesc_code));
      g_tlsdesc_static = mem;
    }
  }
  *out_api = &g_native_api;
  *out_bind = NativeBind;
  return true;
}

#else

bool InitDynarmicBridge(const gesher_arm64_host* /*host*/, const gesher_arm64_api** /*out_api*/,
                        void* (**/*out_bind*/)(void*)) {
  return false;
}

#endif

}  // namespace gesher::arm64
