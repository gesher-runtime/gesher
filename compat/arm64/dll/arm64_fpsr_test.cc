// CI-only actual FPSR flags, arithmetic, nested callbacks and native fenv.
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
constexpr uint32_t kQc = 0x08000000;
const gesher_arm64_api* api;
const void* code;
std::atomic<int> failures{0};
void Check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL  arm64 FPSR: %s\n", what); ++failures; }
}
uint64_t Call(uint64_t operation, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
  uint64_t args[] = {operation, a, b, c};
  return api->call(code, args, 4);
}
int One(const char* name, int argument) {
  const void* entry = api->guest_symbol(name);
  if (!entry) { Check(false, "guest status entry exists"); return -1; }
  uint64_t value = static_cast<uint64_t>(static_cast<int64_t>(argument));
  return static_cast<int32_t>(api->call(entry, &value, 1));
}
int Flag(const char* name, uint32_t* flag, int mask) {
  const void* entry = api->guest_symbol(name);
  if (!entry) { Check(false, "guest flag pointer entry exists"); return -1; }
  uint64_t values[] = {reinterpret_cast<uint64_t>(flag), static_cast<uint64_t>(static_cast<int64_t>(mask))};
  return static_cast<int32_t>(api->call(entry, values, 2));
}
void* Resolve(const char*) { return nullptr; }
int Describe(const void*, const char**, const void**, const char**, const void**) { return 0; }
struct NativeState { int mode, flags; unsigned mxcsr; };
NativeState Native() { return {std::fegetround(), std::fetestexcept(FE_ALL_EXCEPT), _mm_getcsr()}; }
bool Same(NativeState before) {
  NativeState after = Native();
  return before.mode == after.mode && before.flags == after.flags && before.mxcsr == after.mxcsr;
}
NativeState original_native, outer_native;
uint64_t SYSV Inner(uint64_t expected, uint64_t replacement) {
  Check(Same(outer_native), "inner host inherits native flags and rounding");
  Check(Call(4) == expected, "third guest level inherits actual FPSR");
  Check(Call(5, replacement) == replacement, "third guest directly changes FPSR");
  std::fesetround(FE_TOWARDZERO);
  std::feraiseexcept(FE_INVALID);
  return replacement;
}
uint64_t SYSV Outer(uint64_t expected, uint64_t replacement) {
  Check(Same(original_native), "outer callback runs in original host fenv");
  std::fesetround(FE_DOWNWARD);
  std::feraiseexcept(FE_OVERFLOW);
  outer_native = Native();
  Check(Call(4) == expected, "nested guest inherits parent FPSR independently of host flags");
  Check(Call(2, reinterpret_cast<uint64_t>(&Inner), expected, replacement) == replacement,
        "nested status callback completes");
  Check(Same(outer_native), "inner callback restores outer host flags and rounding");
  Check(Call(4) == replacement, "inner status changes persist through guest nesting levels");
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
  api->add_code(code, bytes.size(), "NDK FPCR/FPSR instructions");
  std::fenv_t saved;
  std::fegetenv(&saved);
  std::feclearexcept(FE_ALL_EXCEPT);
  std::fesetround(FE_UPWARD);
  std::feraiseexcept(FE_DIVBYZERO);
  original_native = Native();
  Check(Call(1, 0) == 0 && Call(5, kQc | 0x9f) == (kQc | 0x9f), "direct register writes reach real backend");
  Check(One("fetestexcept", -1) == 0x9f && One("fetestexcept", 0x48000080) == 0x80,
        "test masks include denormal and exclude QC/unknown bits");
  struct { uint32_t before, flag, after; } flags{0x12345678, UINT32_MAX, 0x87654321};
  Check(Flag("fegetexceptflag", &flags.flag, 0x48000081) == 0 && flags.flag == 0x81 &&
        flags.before == 0x12345678 && flags.after == 0x87654321 && Call(4) == (kQc | 0x9f),
        "getflag stores exactly four bytes without changing FPSR");
  flags.flag = 0x40000080;
  Check(Flag("fesetexceptflag", &flags.flag, 0x48000081) == 0 && Call(4) == (kQc | 0x9e) &&
        flags.flag == 0x40000080, "setflag masks and preserves its source and other status");
  Check(One("feclearexcept", 0x48000080) == 0 && Call(4) == (kQc | 0x1e), "partial clear preserves QC");
  Check(One("feraiseexcept", 0x48000081) == 0 && Call(4) == (kQc | 0x9f), "raise sets status without changing QC");
  Check(One("feclearexcept", INT32_MIN) == 0 && Call(4) == (kQc | 0x9f), "unknown mask bits have no effect");
  Check(One("feclearexcept", -1) == 0 && Call(4) == kQc && Call(0) == 0, "all clear preserves QC and FPCR");
  Check(Same(original_native), "native flags/rounding restored after every status operation");
  if (failures == 0) std::puts("PASS  arm64 FPSR: real status, pinned masks, denormal, four-byte flags and preserved QC");
  struct Operation { uint64_t operation, left, right, control, flags; } operations[] = {
      {6, UINT64_C(0x3ff0000000000000), 0, 0, 2},
      {6, 0, 0, 0, 1},
      {7, UINT64_C(0x7fefffffffffffff), UINT64_C(0x4000000000000000), 0, 0x14},
      {7, UINT64_C(0x0010000000000000), UINT64_C(0x0010000000000000), 0, 0x18},
      {3, UINT64_C(0x3ff0000000000000), UINT64_C(0x3ca0000000000000), 0, 0x10},
      {3, 1, UINT64_C(0x3ff0000000000000), UINT64_C(1) << 24, 0x80}};
  for (auto op : operations) {
    Call(1, op.control);
    Call(5, 0);
    (void)Call(op.operation, op.left, op.right);
    Check(Call(4) == op.flags && One("fetestexcept", -1) == static_cast<int>(op.flags),
          "actual FDIV/FMUL/FADD produces exception status, including IDC");
    Check(Same(original_native), "real guest exception arithmetic preserves native fenv");
  }
  Call(1, 0);
  Call(5, 0);
  Check(Call(8) == kQc && One("fetestexcept", -1) == 0, "actual SQADD sets QC outside the C exception mask");
  if (failures == 0) std::puts("PASS  arm64 FPSR: real FDIV FMUL FADD and saturating SIMD produce all six flags and QC");
  uint64_t before = kQc | 0x81, replacement = 0x92;
  Call(5, before);
  Check(Call(2, reinterpret_cast<uint64_t>(&Outer), before, replacement) == replacement && Call(4) == replacement,
        "nested status changes persist on outer guest return");
  Check(Same(original_native), "native exception flags and rounding restored after nested callbacks");
  if (failures == 0) std::puts("PASS  arm64 FPSR: direct MSR and three guest levels inherit/persist status and restore native fenv");
  std::vector<std::thread> threads;
  for (uint64_t i = 0; i < 4; ++i) threads.emplace_back([&, i] {
    std::feclearexcept(FE_ALL_EXCEPT);
    std::fesetround(FE_DOWNWARD);
    std::feraiseexcept(i & 1 ? FE_INEXACT : FE_INVALID);
    NativeState native = Native();
    Check(Call(4) == 0, "new foreign host thread has independent default guest FPSR");
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
      uint64_t own = kQc | (UINT64_C(1) << i);
      Check(Call(5, own) == own && One("fetestexcept", -1) == static_cast<int>(own & 0x9f),
            "guest status remains local across concurrent entries");
      Check(Same(native), "each host thread retains its own native flags and rounding");
      std::this_thread::yield();
    }
  });
  for (auto& thread : threads) thread.join();
  Check(Call(4) == replacement && Same(original_native), "workers preserve caller guest and native state");
  if (failures == 0) std::puts("PASS  arm64 FPSR: concurrent guest state is isolated and every native host fenv is restored");
  std::fesetenv(&saved);
  std::printf("%d test(s) failed\n", failures.load());
  return failures == 0 ? 0 : 1;
}
