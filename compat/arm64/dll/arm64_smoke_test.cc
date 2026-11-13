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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

// A host function that calls back a callback (System V), like qsort's
// comparator: when the callback is guest code, the call faults (DEP) and the
// translator redirects it into the guest.
using Callback = uint64_t(__attribute__((sysv_abi))*)(uint64_t);
extern "C" __attribute__((sysv_abi)) uint64_t host_apply(Callback cb, uint64_t x) {
  return cb(x);
}

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

// Like GuestPage but readable and NOT executable, as the real ELF loader maps a
// guest object's code: a host call to it faults (DEP) and the translator's
// vectored handler redirects it into the guest.
void* GuestPageNX(const uint32_t* code, size_t n) {
  void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  std::memcpy(p, code, n * 4);
  return p;
}

int g_test_errno = 0;
extern "C" __attribute__((sysv_abi)) int* test_errno() { return &g_test_errno; }
void* g_resolve(const char* name) {
  return std::strcmp(name, "__errno") == 0 ? reinterpret_cast<void*>(&test_errno) : nullptr;
}
int g_describe(const void*, const char**, const void**, const char**, const void**) { return 0; }

}  // namespace

int main(int argc, char** argv) {
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
  Check("null host rejected before initialization", init(nullptr) == nullptr);
  for (uint32_t version : {0u, 1u, 2u, GESHER_ARM64_API_VERSION + 1u}) {
    gesher_arm64_host incompatible = {version, g_resolve, g_describe};
    Check("incompatible host va_list ABI rejected", init(&incompatible) == nullptr);
  }
  const gesher_arm64_api* api = init(&host);
  Check("translator initialised", api != nullptr && api->version == GESHER_ARM64_API_VERSION);
  if (!api) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }

  // No private DNS bridges are supplied by this host. Exercise the actual
  // guest bindings and ensure they fail before touching caller-owned memory.
  for (const char* name : {"ns_name_ntop", "ns_name_uncompress", "dn_expand"}) {
    void* fn = api->guest_symbol(name);
    unsigned char guard[16];
    std::memset(guard, 0xa5, sizeof(guard));
    uint64_t args[] = {reinterpret_cast<uint64_t>(guard),
                       reinterpret_cast<uint64_t>(guard),
                       reinterpret_cast<uint64_t>(guard),
                       reinterpret_cast<uint64_t>(guard), sizeof(guard), 0};
    g_test_errno = 0;
    uint64_t result = fn ? api->call(fn, args, 6) : 0;
    bool intact = true;
    for (unsigned char byte : guard) intact &= byte == 0xa5;
    Check(name, fn && result == UINT64_MAX && g_test_errno == 38 && intact,
          "missing ARM64 DNS bridge must fail with ENOSYS");
  }

  // `arm64_smoke_test crash-segv|crash-ill|crash-jump`: a faulting guest, which
  // must end the process with an Android-style "Fatal signal" report rather
  // than hang (tracks/47-arm64.ps1 runs these with a timeout).
  if (argc > 1) {
    std::string mode = argv[1];
    uint32_t segv[] = {MovzX(1, 0), 0xF9400020u /* ldr x0, [x1] */, kRet};
    uint32_t ill[] = {0x00000000u /* udf #0 */, kRet};
    uint32_t jump[] = {MovzX(16, 0x40), kBlrX16, kRet};
    const uint32_t* code = mode == "crash-segv" ? segv : mode == "crash-ill" ? ill : jump;
    size_t n = mode == "crash-segv" ? 3 : mode == "crash-ill" ? 2 : 3;
    void* fn = GuestPage(code, n);
    api->add_code(fn, n * 4, "crash");
    api->call(fn, nullptr, 0);
    std::printf("the faulting guest returned (it should not have)\n");
    return 3;
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

  // 5. Host -> guest callback via the DEP-fault redirect: a host function calls
  //    a guest callback pointer directly (as qsort calls its comparator). The
  //    guest's pages are non-executable, so the call faults and the translator
  //    enters the guest. The guest callback here doubles its argument.
  {
    uint32_t code[] = {0xD37FF800u /* lsl x0, x0, #1 */, kRet};
    void* cb = GuestPageNX(code, 2);
    api->add_code(cb, sizeof(code), "double_cb");
    uint64_t r = host_apply(reinterpret_cast<Callback>(cb), 21);
    Check("host->guest callback through the DEP-fault redirect (21*2)", r == 42);
  }

  // 6. Performance: a CPU-bound guest loop (sum of 0..n-1) timed against the
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
    // The same loop natively, kept in registers like the guest's (the empty
    // asm stops the compiler from folding it into a closed form; a volatile
    // accumulator would add a store per iteration and flatter the guest).
    uint64_t h = 0;
    for (uint64_t i = 0; i < n; ++i) {
      h += i;
      asm volatile("" : "+r"(h) : "r"(i));
    }
    auto t2 = std::chrono::steady_clock::now();
    double gms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double hms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    Check("CPU-bound guest loop matches native result", g == h);
    std::printf("INFO  arm64 perf: %llu-iteration loop: guest %.1f ms, native %.1f ms, "
                "overhead x%.1f\n",
                static_cast<unsigned long long>(n), gms, hms, hms > 0 ? gms / hms : 0.0);
  }

  // 7. Atomics: an LL/SC increment loop (ldaxr/stlxr, what outline atomics
  //    and std::atomic compile to without LSE) on four threads at once. The
  //    store-exclusive goes through the global monitor and a host CAS.
  {
    //   L: ldaxr w2,[x0]; add w2,w2,#1; stlxr w3,w2,[x0]; cbnz w3,L;
    //      subs x1,x1,#1; b.ne L; ret
    uint32_t code[] = {0x885FFC02u, 0x11000442u, 0x8803FC02u, 0x35FFFFA3u,
                       0xF1000421u, 0x54FFFF61u, kRet};
    void* fn = GuestPage(code, 7);
    api->add_code(fn, sizeof(code), "atomic_inc");
    static uint32_t counter = 0;
    static void* s_fn = fn;
    static const gesher_arm64_api* s_api = api;
    auto worker = [](void*) -> DWORD {
      uint64_t args[2] = {reinterpret_cast<uint64_t>(&counter), 100000};
      s_api->call(s_fn, args, 2);
      return 0;
    };
    HANDLE th[4];
    for (HANDLE& h : th) h = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    WaitForMultipleObjects(4, th, TRUE, 60000);
    for (HANDLE h : th) CloseHandle(h);
    Check("4 threads x 100000 guest LL/SC increments == 400000", counter == 400000);

    // 8. More guest threads than the monitor once had processor ids for (64):
    //    each thread's Jit must still have its own reservation and saved LDXR
    //    value. Two threads sharing an id overwrote each other's, and a
    //    store-exclusive then succeeded with the other thread's stale value:
    //    increments were lost.
    static uint32_t many = 0;
    static uint32_t warmup = 0;
    static std::atomic<int> ready{0};
    static HANDLE start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto many_worker = [](void*) -> DWORD {
      uint64_t warm[2] = {reinterpret_cast<uint64_t>(&warmup), 1};
      s_api->call(s_fn, warm, 2);  // this thread's Jit exists: all start together
      ready.fetch_add(1);
      WaitForSingleObject(start, INFINITE);
      uint64_t args[2] = {reinterpret_cast<uint64_t>(&many), 1000};
      s_api->call(s_fn, args, 2);
      return 0;
    };
    constexpr int kMany = 128;
    HANDLE mth[kMany];
    for (HANDLE& h : mth) h = CreateThread(nullptr, 0, many_worker, nullptr, 0, nullptr);
    while (ready.load() < kMany) Sleep(1);
    SetEvent(start);
    for (int i = 0; i < kMany; i += MAXIMUM_WAIT_OBJECTS) {
      int n = kMany - i < MAXIMUM_WAIT_OBJECTS ? kMany - i : MAXIMUM_WAIT_OBJECTS;
      WaitForMultipleObjects(n, mth + i, TRUE, 120000);
    }
    for (HANDLE h : mth) CloseHandle(h);
    std::printf("INFO  %d threads x 1000 LL/SC increments: %u\n", kMany, many);
    Check("128 threads x 1000 guest LL/SC increments == 128000", many == kMany * 1000);
  }

  // 9. A WFE spin-wait (LDAXR; CBNZ; WFE; B) ends when another thread stores
  //    to the word with a plain store (no event: WFE's bounded wait), and
  //    sleeps rather than spins meanwhile.
  //   L: ldaxr w1,[x0]; cbnz w1,done; wfe; b L; done: mov w0,w1; ret
  {
    uint32_t code[] = {0x885FFC01u, 0x35000061u, 0xD503205Fu, 0x17FFFFFDu, 0x2A0103E0u, kRet};
    void* fn = GuestPage(code, 6);
    api->add_code(fn, sizeof(code), "wfe_wait");
    static volatile uint32_t flag = 0;
    HANDLE setter = CreateThread(
        nullptr, 0,
        [](void*) -> DWORD {
          Sleep(300);
          flag = 7;
          return 0;
        },
        nullptr, 0, nullptr);
    FILETIME c, e, k0, u0, k1, u1;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k0, &u0);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t args[1] = {reinterpret_cast<uint64_t>(&flag)};
    uint64_t r = api->call(fn, args, 1);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    GetThreadTimes(GetCurrentThread(), &c, &e, &k1, &u1);
    auto ticks = [](const FILETIME& f) {
      return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    double cpu_ms = static_cast<double>(ticks(u1) - ticks(u0) + ticks(k1) - ticks(k0)) / 1e4;
    WaitForSingleObject(setter, INFINITE);
    CloseHandle(setter);
    std::printf("INFO  WFE wait: %.0f ms elapsed, %.0f ms CPU\n", ms, cpu_ms);
    Check("WFE spin-wait ends on a plain store (returns 7)", r == 7 && ms < 5000);
    Check("WFE spin-wait mostly sleeps (CPU < half the wait)", cpu_ms < ms / 2);
  }

  std::printf("\n");
  if (failures == 0) {
    std::printf("arm64_smoke_test all passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", failures);
  return 1;
}
