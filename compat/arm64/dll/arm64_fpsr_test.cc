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
#include <iomanip>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include "../../../probes/arm64/fpsr/vectors.h"

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
void Trace(const char* phase, uint64_t expected, uint64_t observed, int queried = -1) {
  uint64_t control = Call(0);
  NativeState host = Native();
  std::printf("INFO arm64 FPSR: %s expected=0x%08llx observed=0x%08llx query=0x%08x "
              "fpcr=0x%08llx host_mode=0x%x host_flags=0x%x host_mxcsr=0x%08x\n",
              phase, static_cast<unsigned long long>(expected), static_cast<unsigned long long>(observed),
              static_cast<unsigned>(queried), static_cast<unsigned long long>(control),
              static_cast<unsigned>(host.mode), static_cast<unsigned>(host.flags), host.mxcsr);
}
NativeState original_native, outer_native;
uint64_t SYSV Inner(uint64_t expected, uint64_t replacement) {
  Check(Same(outer_native), "inner host inherits native flags and rounding");
  uint64_t observed = Call(4);
  Trace("third guest level", expected, observed);
  Check(observed == expected, "third guest level inherits actual FPSR");
  Call(5, expected);
  Trace("identical warm third-level MRS", expected, Call(4));
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
  uint64_t observed = Call(4);
  Trace("second guest level", expected, observed);
  Check(observed == expected, "nested guest inherits parent FPSR independently of host flags");
  Call(5, expected);
  Trace("identical warm second-level MRS", expected, Call(4));
  Check(Call(2, reinterpret_cast<uint64_t>(&Inner), expected, replacement) == replacement,
        "nested status callback completes");
  Check(Same(outer_native), "inner callback restores outer host flags and rounding");
  Check(Call(4) == replacement, "inner status changes persist through guest nesting levels");
  return replacement;
}

void CompareIndependentMatrix(const char* filename) {
  struct Restore {
    uint64_t control = Call(0), status = Call(4);
    std::fenv_t native;
    Restore() { std::fegetenv(&native); }
    ~Restore() { Call(1, control); Call(5, status); std::fesetenv(&native); }
  } restore;
  std::ifstream input(filename);
  std::string header;
  if (!std::getline(input, header) || header != "ARM64_FPSR_QEMU_V1") {
    Check(false, "the independent QEMU matrix exists with the expected schema");
    return;
  }
  size_t rows = 0, mismatches = 0;
  size_t numeric_mismatches = 0, status_mismatches = 0, setup_mismatches = 0;
  size_t query_mismatches = 0, native_mismatches = 0, missing_idc = 0, extra_inexact = 0, other_status = 0;
  auto compare = [&](uint64_t operation, uint64_t width, uint64_t control, uint64_t left,
                     uint64_t right, uint64_t seed, uint64_t selected_operation) {
    uint64_t fields[8];
    for (uint64_t& field : fields) if (!(input >> std::hex >> field)) {
      Check(false, "independent QEMU matrix is complete and every field is hexadecimal");
      return false;
    }
    if (fields[0] != operation || fields[1] != width || fields[2] != control ||
        fields[3] != left || fields[4] != right || fields[5] != seed) {
      Check(false, "every independent reference tuple matches the original source inputs");
      return false;
    }
    NativeState native = Native();
    uint64_t selected = Call(1, control), initialized = Call(5, seed);
    uint64_t actual = Call(selected_operation, left, right);
    uint64_t status = Call(4);
    int queried = One("fetestexcept", -1);
    uint64_t remaining_control = Call(0);
    bool setup_matches = selected == control && initialized == seed && remaining_control == control;
    bool query_matches = queried == static_cast<int>(status & 0x9f);
    bool native_matches = Same(native);
    numeric_mismatches += actual != fields[6];
    setup_mismatches += !setup_matches;
    query_mismatches += !query_matches;
    native_mismatches += !native_matches;
    if (status != fields[7]) {
      ++status_mismatches;
      if ((fields[7] ^ status) == 0x80 && !(status & 0x80)) ++missing_idc;
      else if ((fields[7] ^ status) == 0x10 && (status & 0x10)) ++extra_inexact;
      else ++other_status;
    }
    bool matches = setup_matches && actual == fields[6] && status == fields[7] && query_matches && native_matches;
    ++rows;
    if (!matches) {
      if (mismatches < 24) std::printf(
          "INFO arm64 FPSR matrix: row=%zu op=%llu width=%llu fpcr=%08llx left=%016llx right=%016llx seed=%08llx "
          "expected_result=%016llx actual_result=%016llx expected_status=%08llx actual_status=%08llx query=%08x\n",
          rows, static_cast<unsigned long long>(operation), static_cast<unsigned long long>(width),
          static_cast<unsigned long long>(control), static_cast<unsigned long long>(left),
          static_cast<unsigned long long>(right), static_cast<unsigned long long>(seed),
          static_cast<unsigned long long>(fields[6]), static_cast<unsigned long long>(actual),
          static_cast<unsigned long long>(fields[7]), static_cast<unsigned long long>(status), static_cast<unsigned>(queried));
      ++mismatches;
    }
    return true;
  };
  const uint64_t operations64[] = {3, 13, 7, 6};
  for (uint64_t mode = 0; mode < 4; ++mode) for (uint64_t dn = 0; dn < 2; ++dn) for (uint64_t fz = 0; fz < 2; ++fz) {
    uint64_t control = (mode << 22) | (dn << 25) | (fz << 24);
    for (uint64_t width : {32, 64}) for (uint64_t operation = 0; operation < 4; ++operation) {
      for (const auto& vector : ArmFpsrVectors) for (uint64_t seed : {UINT64_C(0), UINT64_C(0x08000081)}) {
        uint64_t left = width == 32 ? vector.left32 : vector.left64;
        uint64_t right = width == 32 ? vector.right32 : vector.right64;
        if (!compare(operation, width, control, left, right, seed,
                     width == 32 ? 9 + operation : operations64[operation])) return;
      }
    }
  }
  size_t baseline_mismatches = mismatches;
  for (uint64_t mode = 0; mode < 4; ++mode) for (uint64_t dn = 0; dn < 2; ++dn) {
    uint64_t control = (mode << 22) | (dn << 25) | (UINT64_C(1) << 24);
    for (uint64_t width : {32, 64}) {
      uint64_t minimum = width == 32 ? UINT64_C(0x00800000) : UINT64_C(0x0010000000000000);
      for (uint64_t operation = ARM_FPSR_SEEDED_MUL; operation <= ARM_FPSR_MUL_THEN_ADD; ++operation) {
        uint64_t selected = operation == ARM_FPSR_SEEDED_MUL ? (width == 32 ? 11 : 7) :
                            (operation == ARM_FPSR_ADD_THEN_MUL ? 14 : 16) + (width == 32 ? 1 : 0);
        for (uint64_t seed : {UINT64_C(0), UINT64_C(0x08000081), UINT64_C(0x08000091)}) {
          if (!compare(operation, width, control, minimum, minimum, seed, selected)) return;
        }
      }
    }
  }
  size_t cumulative_mismatches = mismatches - baseline_mismatches;
  for (uint64_t mode = 0; mode < 4; ++mode) for (uint64_t dn = 0; dn < 2; ++dn) for (uint64_t fz = 0; fz < 2; ++fz) {
    uint64_t control = (mode << 22) | (dn << 25) | (fz << 24);
    for (uint64_t width : {32, 64}) for (uint64_t operation = 0; operation < 4; ++operation) {
      for (const auto& vector : ArmFpsrBoundaryVectors) for (uint64_t seed : {UINT64_C(0), UINT64_C(0x08000081)}) {
        uint64_t left = width == 32 ? vector.left32 : vector.left64;
        uint64_t right = width == 32 ? vector.right32 : vector.right64;
        if (!compare(operation, width, control, left, right, seed,
                     width == 32 ? 9 + operation : operations64[operation])) return;
      }
    }
  }
  std::string extra;
  bool complete = rows == 10384 && !(input >> extra);
  Check(complete, "all original 8192, cumulative144 and boundary2048 independent rows are consumed with no trailing data");
  std::printf("INFO arm64 FPSR matrix classification: numeric=%zu status=%zu setup=%zu query=%zu native=%zu "
              "missing_IDC=%zu extra_IXC=%zu other_status=%zu\n",
              numeric_mismatches, status_mismatches, setup_mismatches, query_mismatches,
              native_mismatches, missing_idc, extra_inexact, other_status);
  if (mismatches != 0) {
    std::printf("INFO arm64 FPSR matrix: %zu mismatching numeric/status cases of %zu\n", mismatches, rows);
    Check(false, "actual scalar instruction bits and status match independent pinned QEMU");
  }
  if (complete && baseline_mismatches == 0) {
    std::puts("PASS  arm64 FPSR: 8192 independent QEMU numeric/status cases, scalar32/64 add sub mul div, all modes DN/FZ and NaN/inf/subnormal rules");
  }
  if (complete && cumulative_mismatches == 0) {
    std::puts("PASS  arm64 FPSR: 144 independent QEMU cumulative cases preserve seeded and same-block IXC before and after FZ underflow");
  }
  if (complete && mismatches == baseline_mismatches + cumulative_mismatches) {
    std::puts("PASS  arm64 FPSR: 2048 independent QEMU normal-operand boundary cases preserve numeric bits and status near smallest normal");
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 3) return 2;
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
  Trace("initial guest/native state", 0, Call(4));
  Call(5, 0);
  uint64_t warm_initial = Call(4);
  Trace("identical warm initial MRS", 0, warm_initial);
  Check(warm_initial == 0, "warm initial MRS leaves the explicitly cleared status unchanged");
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
    uint64_t observed = Call(4);
    int queried = One("fetestexcept", -1);
    Trace(op.operation == 6 ? "FDIV" : op.operation == 7 ? "FMUL" : "FADD", op.flags, observed, queried);
    Check(observed == op.flags && queried == static_cast<int>(op.flags),
          "actual FDIV/FMUL/FADD produces exception status, including IDC");
    if (op.operation == 6 && op.left != 0) {
      Call(5, 0);
      (void)Call(op.operation, op.left, op.right);
      uint64_t warmed = Call(4);
      Trace("identical warm FDIV", op.flags, warmed, One("fetestexcept", -1));
      Check(warmed == op.flags, "identical warm FDIV has only its actual exception flag");
    }
    Check(Same(original_native), "real guest exception arithmetic preserves native fenv");
  }
  Call(1, 0);
  Call(5, 0);
  uint64_t saturated = Call(8);
  uint64_t saturation_status = Call(4);
  int saturation_query = One("fetestexcept", -1);
  Trace("SQADD returned status", kQc, saturated, saturation_query);
  Trace("SQADD persisted status", kQc, saturation_status, saturation_query);
  Check(saturated == kQc && saturation_query == 0, "actual SQADD sets QC outside the C exception mask");
  Call(5, 0);
  uint64_t warm_saturation = Call(8);
  Trace("identical warm SQADD", kQc, warm_saturation, One("fetestexcept", -1));
  Check(warm_saturation == kQc, "identical warm SQADD sets only cumulative saturation");
  if (failures == 0) std::puts("PASS  arm64 FPSR: real FDIV FMUL FADD and saturating SIMD produce all six flags and QC");
  CompareIndependentMatrix(argv[2]);
  uint64_t before = kQc | 0x81, replacement = 0x92;
  Call(5, before);
  Trace("before outer callback", before, Call(4));
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
    uint64_t initial = Call(4);
    Trace("new foreign thread", 0, initial);
    Check(initial == 0, "new foreign host thread has independent default guest FPSR");
    Call(5, 0);
    Trace("identical warm foreign-thread MRS", 0, Call(4));
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
