// CI-only real translated FPCR, arithmetic, nested callback and host fenv test.
#include <gesher/arm64_translator.h>
#include <windows.h>
#include <xmmintrin.h>

#include <atomic>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#define SYSV __attribute__((sysv_abi))
namespace {
const gesher_arm64_api* api;
const void* code;
std::atomic<int> failures{0};
void Check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL  arm64 FPCR: %s\n", what); ++failures; }
}
uint64_t Call(uint64_t operation, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
  uint64_t args[] = {operation, a, b, c};
  return api->call(code, args, 4);
}
void* Resolve(const char*) { return nullptr; }
int Describe(const void*, const char**, const void**, const char**, const void**) { return 0; }

uint64_t SYSV Inner(uint64_t expected, uint64_t replacement) {
  Check(std::fegetround() == FE_DOWNWARD, "inner host inherits native callback fenv");
  Check(Call(0) == expected, "third guest entry inherits actual parent FPCR");
  Check(Call(1, replacement) == replacement, "third guest changes FPCR by MSR");
  std::fesetround(FE_TOWARDZERO);
  return replacement;
}
uint64_t SYSV Outer(uint64_t expected, uint64_t replacement) {
  Check(std::fegetround() == FE_UPWARD, "host callback runs in original host rounding mode");
  std::fesetround(FE_DOWNWARD);
  Check(Call(0) == expected, "nested guest inherits outer FPCR independently of host mode");
  Check(Call(2, reinterpret_cast<uint64_t>(&Inner), expected, replacement) == replacement,
        "guest-host-guest-host-guest returns its actual result");
  Check(std::fegetround() == FE_DOWNWARD, "inner host callback restores outer host mode");
  Check(Call(0) == replacement, "inner MSR persists through both guest nesting levels");
  return replacement;
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 2) return 2;
  HMODULE dll = LoadLibraryA(GESHER_ARM64_DLL);
  if (!dll) return 2;
  auto init = reinterpret_cast<gesher_arm64_init_fn>(
      reinterpret_cast<void*>(GetProcAddress(dll, GESHER_ARM64_INIT_SYMBOL)));
  if (!init) return 2;
  gesher_arm64_host host{GESHER_ARM64_API_VERSION, Resolve, Describe};
  api = init(&host);
  if (!api || api->version != GESHER_ARM64_API_VERSION) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
  if (bytes.empty()) return 2;
  void* page = VirtualAlloc(nullptr, bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!page) return 2;
  std::memcpy(page, bytes.data(), bytes.size());
  code = page;
  api->add_code(code, bytes.size(), "NDK FPCR instructions");
  const void* setround = api->guest_symbol("fesetround");
  const void* getround = api->guest_symbol("fegetround");
  if (!setround || !getround) return 2;
  std::fenv_t original;
  std::fegetenv(&original);
  std::fesetround(FE_UPWARD);
  unsigned host_mxcsr = _mm_getcsr();
  const uint64_t expected_positive[] = {UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000001),
                                      UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000)};
  const uint64_t expected_negative[] = {UINT64_C(0xbff0000000000000), UINT64_C(0xbff0000000000000),
                                      UINT64_C(0xbff0000000000001), UINT64_C(0xbff0000000000000)};
  for (uint64_t mode = 0; mode < 4; ++mode) {
    uint64_t seed = UINT64_C(0x03000000) | ((3 - mode) << 22);
    Check(Call(1, seed) == seed, "direct MSR writes non-rounding FPCR bits");
    Check(api->call(setround, &mode, 1) == 0, "all four pinned modes accepted");
    Check(api->call(getround, nullptr, 0) == mode, "mode getter reads real FPCR");
    Check(Call(0) == (UINT64_C(0x03000000) | (mode << 22)), "setter preserves other FPCR bits");
    Check(Call(3, UINT64_C(0x3ff0000000000000), UINT64_C(0x3ca0000000000000)) == expected_positive[mode],
          "translated positive FADD uses FPCR, including halfway rounding");
    Check(Call(3, UINT64_C(0xbff0000000000000), UINT64_C(0xbca0000000000000)) == expected_negative[mode],
          "translated negative FADD uses FPCR");
    Check(std::fegetround() == FE_UPWARD && _mm_getcsr() == host_mxcsr, "native host fenv restored");
  }
  if (failures == 0) std::puts("PASS  arm64 FPCR: four modes, real positive/negative arithmetic and preserved FPCR bits");
  uint64_t before = Call(0);
  for (int mode : {-1, 4, INT32_MIN, INT32_MAX}) {
    uint64_t argument = static_cast<uint64_t>(static_cast<int64_t>(mode));
    Check(static_cast<int32_t>(api->call(setround, &argument, 1)) == -1 && Call(0) == before,
          "invalid modes fail without modifying actual state");
  }
  if (failures == 0) std::puts("PASS  arm64 FPCR: invalid mode rejection preserves actual state");
  uint64_t replacement = UINT64_C(0x02000000) | (UINT64_C(2) << 22);
  Check(Call(2, reinterpret_cast<uint64_t>(&Outer), before, replacement) == replacement,
        "nested callback completes");
  Check(Call(0) == replacement, "nested FPCR changes persist on outer return");
  Check(std::fegetround() == FE_UPWARD && _mm_getcsr() == host_mxcsr, "nested callbacks restore full native MXCSR");
  if (failures == 0) std::puts("PASS  arm64 FPCR: direct MSR and nested guest-host callbacks inherit/persist state and restore host fenv");
  std::vector<std::thread> threads;
  for (uint64_t mode = 0; mode < 4; ++mode) threads.emplace_back([&, mode] {
    std::fesetround(FE_DOWNWARD);
    for (unsigned i = 0; i < 32; ++i) {
      uint64_t fpcr = mode << 22;
      Check(Call(1, fpcr) == fpcr && api->call(getround, nullptr, 0) == mode,
            "independent thread FPCR survives repeated guest entries");
      Check(Call(3, UINT64_C(0x3ff0000000000000), UINT64_C(0x3ca0000000000000)) == expected_positive[mode],
            "concurrent translated arithmetic uses each thread's mode");
      Check(std::fegetround() == FE_DOWNWARD, "each thread retains its native host rounding mode");
      std::this_thread::yield();
    }
  });
  for (auto& thread : threads) thread.join();
  Check(Call(0) == replacement, "workers do not change caller FPCR");
  if (failures == 0) std::puts("PASS  arm64 FPCR: concurrent guest thread isolation and native host fenv restoration");
  std::fesetenv(&original);
  std::printf("%d test(s) failed\n", failures.load());
  return failures == 0 ? 0 : 1;
}
