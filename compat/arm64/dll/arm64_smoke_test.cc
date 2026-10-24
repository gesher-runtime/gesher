// gesher arm64 translator smoke test (Phase 7, step 1). Loads gesher_arm64.dll
// the way the real loader does (LoadLibrary + gesher_arm64_init) and runs
// hand-assembled AArch64 functions in writable, executable guest memory:
//  - host -> guest through call() and through a host_entry() System V stub;
//  - guest -> host through a bound trampoline (the guest calls a host function
//    that adds its two arguments), so the whole ABI round trip is exercised.
//
// Prints "arm64_smoke_test all passed" and exits 0 on success. No NDK or AOSP.

#include <gesher/arm64_translator.h>

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;
void Check(const char* what, bool ok, const char* detail = "") {
  std::printf("%-52s %s%s%s\n", what, ok ? "ok" : "FAIL", *detail ? "  " : "", detail);
  if (!ok) ++failures;
}

// A host function the guest calls: System V, two integer arguments.
extern "C" __attribute__((sysv_abi)) uint64_t host_add(uint64_t a, uint64_t b) { return a + b; }
// A host function with a double argument and double return.
extern "C" __attribute__((sysv_abi)) double host_scale(double x, uint64_t n) { return x * n; }

// AArch64 instruction encodings (little-endian 32-bit words).
constexpr uint32_t kRet = 0xD65F03C0;         // ret
uint32_t MovzX(int rd, uint16_t imm) {         // movz xRd, #imm
  return 0xD2800000u | (uint32_t(imm) << 5) | (rd & 31);
}
uint32_t AddX(int rd, int rn, int rm) {        // add xRd, xRn, xRm
  return 0x8B000000u | ((rm & 31) << 16) | ((rn & 31) << 5) | (rd & 31);
}
constexpr uint32_t kBlrX16 = 0xD63F0200;      // blr x16

void* GuestPage(const uint32_t* code, size_t n) {
  void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  std::memcpy(p, code, n * 4);
  FlushInstructionCache(GetCurrentProcess(), p, n * 4);
  return p;
}

void* g_resolve(const char*) { return nullptr; }
int g_describe(const void*, const char**, const void**, const char**, const void**) { return 0; }

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE dll = LoadLibraryA(GESHER_ARM64_DLL);
  Check("gesher_arm64.dll loaded", dll != nullptr);
  if (!dll) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  auto init = reinterpret_cast<gesher_arm64_init_fn>(
      reinterpret_cast<void*>(GetProcAddress(dll, GESHER_ARM64_INIT_SYMBOL)));
  Check("gesher_arm64_init found", init != nullptr);
  auto bind = reinterpret_cast<void* (*)(void*)>(
      reinterpret_cast<void*>(GetProcAddress(dll, "gesher_arm64_bind")));
  Check("gesher_arm64_bind found", bind != nullptr);
  if (!init || !bind) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }

  gesher_arm64_host host = {GESHER_ARM64_API_VERSION, g_resolve, g_describe};
  const gesher_arm64_api* api = init(&host);
  Check("translator initialised", api != nullptr && api->version == GESHER_ARM64_API_VERSION);
  if (!api) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }

  // 1. host -> guest: a guest function returning 42.
  {
    uint32_t code[] = {MovzX(0, 42), kRet};
    void* fn = GuestPage(code, 2);
    api->add_code(fn, sizeof(code), "ret42");
    uint64_t r = api->call(fn, nullptr, 0);
    Check("guest returns a constant (movz x0,#42; ret)", r == 42,
          r == 42 ? "" : "wrong result");
  }

  // 2. host -> guest with arguments: add x0, x0, x1.
  {
    uint32_t code[] = {AddX(0, 0, 1), kRet};
    void* fn = GuestPage(code, 2);
    api->add_code(fn, sizeof(code), "add");
    uint64_t args[2] = {40, 2};
    uint64_t r = api->call(fn, args, 2);
    Check("guest adds two arguments (add x0,x0,x1; ret)", r == 42);
  }

  // 3. host -> guest through a host_entry System V stub, called as a function.
  {
    uint32_t code[] = {AddX(0, 0, 1), kRet};
    void* fn = GuestPage(code, 2);
    api->add_code(fn, sizeof(code), "add_entry");
    auto stub = reinterpret_cast<uint64_t(__attribute__((sysv_abi))*)(uint64_t, uint64_t)>(
        api->host_entry(fn));
    Check("host_entry returns a stub", stub != nullptr);
    uint64_t r = stub ? stub(100, 23) : 0;
    Check("host_entry stub enters the guest (100+23)", r == 123);
  }

  // 4. guest -> host: the guest calls host_add through a bound trampoline.
  //    x16 = trampoline address (passed in x2); blr x16 with x0,x1 set.
  {
    void* tramp = bind(reinterpret_cast<void*>(&host_add));
    Check("import bound to a guest trampoline", tramp != nullptr);
    // guest: save x30, call the trampoline in x2, restore x30, return:
    //   sub sp,sp,#16; str x30,[sp]; mov x16,x2; blr x16; ldr x30,[sp];
    //   add sp,sp,#16; ret
    uint32_t code[] = {0xD10043FFu, 0xF90003FEu, 0xAA0203F0u, kBlrX16,
                       0xF94003FEu, 0x910043FFu, kRet};
    void* fn = GuestPage(code, 7);
    api->add_code(fn, sizeof(code), "call_host");
    uint64_t args[3] = {30, 12, reinterpret_cast<uint64_t>(tramp)};
    uint64_t r = api->call(fn, args, 3);
    Check("guest calls a host function through a trampoline (30+12)", r == 42);
  }

  // 5. Performance: a CPU-bound guest loop (sum of 0..n-1) timed against the
  //    same loop compiled natively, to record the translation overhead
  //    (docs/arm64.md). Reported as INFO; not a pass/fail gate.
  {
    // guest: x0 = n; sum 0..n-1 in a tight loop; returns the sum.
    //   mov x1,#0; mov x2,#0; L: cmp x2,x0; b.ge E; add x1,x1,x2;
    //   add x2,x2,#1; b L; E: mov x0,x1; ret
    uint32_t code[] = {0xD2800001u, 0xD2800002u, 0xEB00005Fu, 0x5400008Au,
                       0x8B020021u, 0x91000442u, 0x17FFFFFCu, 0xAA0103E0u, kRet};
    void* fn = GuestPage(code, 9);
    api->add_code(fn, sizeof(code), "sumloop");
    const uint64_t n = 200000000ull;
    uint64_t args[1] = {n};
    auto t0 = std::chrono::steady_clock::now();
    uint64_t g = api->call(fn, args, 1);
    auto t1 = std::chrono::steady_clock::now();
    volatile uint64_t h = 0;
    for (uint64_t i = 0; i < n; ++i) h += i;
    auto t2 = std::chrono::steady_clock::now();
    double gms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double hms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    Check("CPU-bound guest loop matches native result", g == h);
    std::printf("INFO  arm64 perf: %llu-iteration loop: guest %.1f ms, native %.1f ms, "
                "overhead x%.1f\n",
                static_cast<unsigned long long>(n), gms, hms, hms > 0 ? gms / hms : 0.0);
  }

  std::printf("\n");
  if (failures == 0) {
    std::printf("arm64_smoke_test all passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", failures);
  return 1;
}
