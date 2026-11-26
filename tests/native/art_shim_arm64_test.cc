// gesher: the arm64 stand-in libart.so and libdexfile.so (compat/bionic/art_shim.cc,
// ADR 0086) loaded by LoadArtShimArm64 and run under the arm64 translator
// (gesher_arm64.dll, next to this executable).
//
//   art_shim_arm64_test <scratch directory>
//
// Their imports are bound to host fakes here that record the System V
// arguments they receive. Each stand-in function is called with a distinct
// value in every argument and the fake must see exactly those values in the
// device's positions:
//  - libdexfile.so's OpenCommon entry, as ART calls it through a host entry
//    (patches/art/0803): System V, the result storage first;
//  - libdexfile.so's OpenCommon itself, by hand-assembled guest code making the
//    device's AAPCS64 call (result storage in X8, error_msg and error_code on
//    the stack), as a shell's inline hook calls the original;
//  - libart.so's DisableVerifier, IsVerificationEnabled and
//    IsVerificationSoftFail (their GOT slots, their arguments untouched, the
//    result returned);
//  - libart.so's LoadClass with ART's five arguments.
// Also: Runtime::instance_ holds the Runtime pointer given, kOatVersion the
// version, both objects are arm64; the imports are not visible to the bionic
// table, the bionic resolver or dlsym once loaded; and a load that fails after
// libdexfile.so loaded keeps it, so a retry loads each object exactly once.
//
// Prints "art_shim_arm64_test all passed" and exits 0 on success.

#include <gesher/arm64_runtime.h>
#include <gesher/art_shim.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#else
#define SYSV
#endif

namespace {

namespace arm64 = gesher::arm64;
namespace elf = gesher::elf;
using namespace gesher::bionic;

int g_failures = 0;

void Check(const char* what, bool ok, const std::string& detail = "") {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ",
              detail.c_str());
  if (!ok) ++g_failures;
}

std::string Hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

// What a fake received, in System V order.
struct Received {
  int calls = 0;
  uint64_t args[11] = {};
};
Received g_disable, g_enabled, g_soft_fail, g_open_common;

void Record(Received* r, std::initializer_list<uint64_t> args) {
  ++r->calls;
  size_t i = 0;
  for (uint64_t a : args) r->args[i++] = a;
}

// The Runtime functions' fakes: `this` and the five registers after it, which
// a forwarder must leave alone. Distinct results identify the GOT slot used.
constexpr uint64_t kEnabledResult = 0x5e;
constexpr uint64_t kSoftFailResult = 0x5f;
SYSV void FakeDisableVerifier(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
  Record(&g_disable, {a0, a1, a2, a3, a4, a5});
}
SYSV uint64_t FakeIsVerificationEnabled(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5) {
  Record(&g_enabled, {a0, a1, a2, a3, a4, a5});
  return kEnabledResult;
}
SYSV uint64_t FakeIsVerificationSoftFail(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5) {
  Record(&g_soft_fail, {a0, a1, a2, a3, a4, a5});
  return kSoftFailResult;
}
// gesher_art_dex_file_loader_open_common's signature: result, container, base,
// size, location, location_checksum, oat_dex_file, verify, verify_checksum,
// error_msg, error_code.
SYSV void FakeOpenCommon(uint64_t result, uint64_t container, uint64_t base, uint64_t size,
                         uint64_t location, uint64_t checksum, uint64_t oat_dex_file,
                         uint64_t verify, uint64_t verify_checksum, uint64_t error_msg,
                         uint64_t error_code) {
  Record(&g_open_common, {result, container, base, size, location, checksum, oat_dex_file, verify,
                          verify_checksum, error_msg, error_code});
}

// Distinct values for OpenCommon's eleven arguments, in the fake's order.
constexpr uint64_t kOpenCommonArgs[11] = {
    0x8888000000000008ull,  // result storage (X8 on a device)
    0x1010000000000001ull,  // container
    0x2020000000000002ull,  // base
    0x3030000000000003ull,  // size
    0x4040000000000004ull,  // location
    0x5050000000000005ull,  // location_checksum
    0x6060000000000006ull,  // oat_dex_file
    0x7070000000000007ull,  // verify
    0x9090000000000009ull,  // verify_checksum
    0xa0a000000000000aull,  // error_msg
    0xb0b000000000000bull,  // error_code
};

std::string Compare(const Received& r, const uint64_t* want, size_t n) {
  std::string detail;
  for (size_t i = 0; i < n; ++i) {
    if (r.args[i] != want[i]) detail += "arg " + std::to_string(i) + " " + Hex(r.args[i]) + " ";
  }
  return detail;
}

// AArch64 encodings for the caller below.
uint32_t LdrLiteral(int rt, int64_t delta) {  // ldr xRt, <pc + delta>
  return 0x58000000u | ((static_cast<uint32_t>(delta / 4) & 0x7ffffu) << 5) | (rt & 31);
}

// Guest code making the device's call to OpenCommon (AAPCS64): the result
// storage in X8, container..verify_checksum in X0-X7, error_msg and error_code
// in the first two stack slots; the values from a literal pool.
void* OpenCommonCaller(const void* open_common) {
  std::vector<uint32_t> code = {
      0xa9bf7bfd,  // stp x29, x30, [sp, #-16]!
      0x910003fd,  // mov x29, sp
      0xd10043ff,  // sub sp, sp, #16
  };
  // The literal pool (quads 0..10: kOpenCommonArgs; 11: the target) follows
  // 3 prologue words, 2 loads, a store, 9 loads, the target's load, the BLR and
  // 3 epilogue words: an even count, so the pool is 8-aligned.
  const size_t pool_word = 3 + 2 + 1 + 9 + 1 + 1 + 3;
  auto ldr = [&](int rt, int quad) {
    const size_t at = code.size();
    code.push_back(LdrLiteral(rt, static_cast<int64_t>(pool_word * 4 + quad * 8) -
                                      static_cast<int64_t>(at * 4)));
  };
  ldr(9, 9);                   // error_msg
  ldr(10, 10);                 // error_code
  code.push_back(0xa9002be9);  // stp x9, x10, [sp]
  ldr(8, 0);                   // result storage
  for (int r = 0; r < 8; ++r) ldr(r, r + 1);  // container .. verify_checksum
  ldr(16, 11);
  code.push_back(0xd63f0200);  // blr x16
  code.push_back(0x910003bf);  // mov sp, x29
  code.push_back(0xa8c17bfd);  // ldp x29, x30, [sp], #16
  code.push_back(0xd65f03c0);  // ret
  if (code.size() != pool_word) return nullptr;
  std::vector<uint64_t> pool(std::begin(kOpenCommonArgs), std::end(kOpenCommonArgs));
  pool.push_back(reinterpret_cast<uint64_t>(open_common));
  void* p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (p == nullptr) return nullptr;
  std::memcpy(p, code.data(), code.size() * 4);
  std::memcpy(static_cast<uint8_t*>(p) + pool_word * 4, pool.data(), pool.size() * 8);
  FlushInstructionCache(GetCurrentProcess(), p, 0x1000);
  arm64::AddCode(p, pool_word * 4, "art_shim_arm64_test OpenCommon caller");
  return p;
}

int CountLoaded(const char* soname) {
  int n = 0;
  elf::ForEachLoaded([&](const elf::PhdrInfo& info) {
    if (std::strcmp(info.name, soname) == 0) ++n;
    return 0;
  });
  return n;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scratch directory>\n", argv[0]);
    return 2;
  }
  InstallBionicShim();
  Check("arm64 translator available", arm64::EnsureLoaded());
  if (!arm64::Available()) return 1;

  namespace fs = std::filesystem;
  const std::string dir = argv[1];
  const std::string version("099", 4);
  void* const runtime = reinterpret_cast<void*>(0x7e57000012345678ull);
  ArtShimArm64Imports imports;
  imports.disable_verifier = reinterpret_cast<void*>(&FakeDisableVerifier);
  imports.is_verification_enabled = reinterpret_cast<void*>(&FakeIsVerificationEnabled);
  imports.is_verification_soft_fail = reinterpret_cast<void*>(&FakeIsVerificationSoftFail);
  imports.open_common = reinterpret_cast<void*>(&FakeOpenCommon);

  // A failure after libdexfile.so loaded: libart.so is held open without
  // sharing, so it can be neither read nor replaced.
  const std::string art_image = ArtShimImageArm64(version);
  const std::string dexfile_image = DexFileShimImageArm64();
  uint64_t fnv = 14695981039346656037ull;
  for (unsigned char c : art_image) fnv = (fnv ^ c) * 1099511628211ull;
  for (unsigned char c : dexfile_image) fnv = (fnv ^ c) * 1099511628211ull;
  char name[32];
  std::snprintf(name, sizeof(name), "%s-%016llx", kArtShimKindArm64,
                static_cast<unsigned long long>(fnv));
  const std::string sub = dir + "/" + name;
  std::error_code ec;
  fs::remove_all(fs::path(sub), ec);
  fs::create_directories(fs::path(sub), ec);
  std::ofstream(sub + "/libart.so", std::ios::binary) << art_image;
  HANDLE held = CreateFileA((sub + "/libart.so").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  Check("libart.so is held without sharing", held != INVALID_HANDLE_VALUE,
        std::to_string(GetLastError()));
  std::string path, error;
  void* open_common_entry = nullptr;
  void* load_class = LoadArtShimArm64(dir, version, runtime, imports, &open_common_entry, &path, &error);
  Check("the load fails while libart.so cannot be written or read", load_class == nullptr, error);
  Check("libdexfile.so loaded before that, libart.so not",
        CountLoaded("libdexfile.so") == 1 && CountLoaded("libart.so") == 0,
        std::to_string(CountLoaded("libdexfile.so")) + "/" + std::to_string(CountLoaded("libart.so")));
  if (held != INVALID_HANDLE_VALUE) CloseHandle(held);

  error.clear();
  load_class = LoadArtShimArm64(dir, version, runtime, imports, &open_common_entry, &path, &error);
  Check("the stand-ins load on a retry", load_class != nullptr && open_common_entry != nullptr,
        path + " " + error);
  if (load_class == nullptr || open_common_entry == nullptr) return 1;
  Check("the retry loaded each object exactly once",
        CountLoaded("libdexfile.so") == 1 && CountLoaded("libart.so") == 1,
        std::to_string(CountLoaded("libdexfile.so")) + "/" + std::to_string(CountLoaded("libart.so")));
  std::string again_path, again_error;
  void* again_entry = nullptr;
  Check("a later call returns the same entries",
        LoadArtShimArm64(dir, version, runtime, imports, &again_entry, &again_path, &again_error) ==
                load_class &&
            again_entry == open_common_entry && again_path == path);

  elf::Library* art = elf::Find("libart.so");
  elf::Library* dexfile = elf::Find("libdexfile.so");
  Check("both objects are arm64", elf::IsAarch64(art) && elf::IsAarch64(dexfile));
  void* instance = art != nullptr ? elf::Lookup(art, kArtRuntimeInstanceSymbol) : nullptr;
  uint64_t instance_value = 0;
  if (instance != nullptr) std::memcpy(&instance_value, instance, 8);
  Check("Runtime::instance_ holds the Runtime pointer",
        instance_value == reinterpret_cast<uint64_t>(runtime), Hex(instance_value));
  void* oat = art != nullptr ? elf::Lookup(art, kArtOatVersionSymbol) : nullptr;
  Check("OatHeader::kOatVersion holds the version",
        oat != nullptr && std::memcmp(oat, version.data(), 4) == 0);
  Check("LoadClass is libart.so's, the entry libdexfile.so's",
        art != nullptr && dexfile != nullptr && elf::Lookup(art, kArtLoadClassSymbol) == load_class &&
            elf::Lookup(dexfile, kArtOpenCommonEntrySymbol) == open_common_entry);

  bool hidden = true;
  for (const char* import : {kArtShimDisableVerifierImport, kArtShimIsVerificationEnabledImport,
                             kArtShimIsVerificationSoftFailImport, kArtShimOpenCommonImport}) {
    hidden = hidden && LookupBionicSymbol(import) == nullptr && elf::ResolveBionic(import) == nullptr &&
             ResolveArtShimImport(import) == nullptr && elf::LookupGlobal(import) == nullptr;
  }
  Check("the imports are in no symbol table once loaded (bionic table, resolver, global)", hidden);

  // libdexfile.so's entry, as ART calls it (System V, result storage first).
  using Entry = void(SYSV*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t);
  auto entry = reinterpret_cast<Entry>(arm64::HostEntry(open_common_entry));
  const uint64_t* a = kOpenCommonArgs;
  if (entry != nullptr) entry(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]);
  Check("the OpenCommon entry reaches ART's OpenCommon once", g_open_common.calls == 1,
        std::to_string(g_open_common.calls));
  Check("... with each argument in its place", Compare(g_open_common, a, 11).empty(),
        Compare(g_open_common, a, 11));

  // OpenCommon itself, called as on a device (AAPCS64, X8, stack arguments).
  g_open_common = {};
  void* open_common = dexfile != nullptr ? elf::Lookup(dexfile, kArtOpenCommonSymbol) : nullptr;
  void* caller = open_common != nullptr ? OpenCommonCaller(open_common) : nullptr;
  Check("guest code calling OpenCommon is set up", caller != nullptr);
  if (caller != nullptr) arm64::Call(caller, nullptr, 0);
  Check("OpenCommon's AAPCS64 call reaches ART's OpenCommon once", g_open_common.calls == 1,
        std::to_string(g_open_common.calls));
  Check("... with X8 first and each argument in its place", Compare(g_open_common, a, 11).empty(),
        Compare(g_open_common, a, 11));

  // The Runtime functions: `this` and five more registers, untouched.
  const uint64_t regs[6] = {reinterpret_cast<uint64_t>(runtime), 0x1111, 0x2222, 0x3333, 0x4444,
                            0x5555};
  using RuntimeFn = uint64_t(SYSV*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  struct Case {
    const char* symbol;
    Received* fake;
    const char* what;
    bool returns;
    uint64_t result;
  };
  const Case cases[] = {
      {kArtDisableVerifierSymbol, &g_disable, "Runtime::DisableVerifier", false, 0},
      {kArtIsVerificationEnabledSymbol, &g_enabled, "Runtime::IsVerificationEnabled", true,
       kEnabledResult},
      {kArtIsVerificationSoftFailSymbol, &g_soft_fail, "Runtime::IsVerificationSoftFail", true,
       kSoftFailResult},
  };
  for (const Case& c : cases) {
    void* guest = art != nullptr ? elf::Lookup(art, c.symbol) : nullptr;
    auto fn = guest != nullptr ? reinterpret_cast<RuntimeFn>(arm64::HostEntry(guest)) : nullptr;
    const int before[3] = {g_disable.calls, g_enabled.calls, g_soft_fail.calls};
    const uint64_t result = fn != nullptr ? fn(regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]) : 0;
    const int after[3] = {g_disable.calls, g_enabled.calls, g_soft_fail.calls};
    int others = 0;
    for (int i = 0; i < 3; ++i) others += after[i] - before[i];
    Check((std::string(c.what) + " reaches its own import once").c_str(),
          c.fake->calls == 1 && others == 1,
          std::to_string(c.fake->calls) + " of " + std::to_string(others));
    Check((std::string(c.what) + ": this and the other registers in place").c_str(),
          Compare(*c.fake, regs, 6).empty(), Compare(*c.fake, regs, 6));
    if (c.returns) {
      Check((std::string(c.what) + " returns the import's result").c_str(), result == c.result,
            Hex(result));
    }
  }

  // LoadClass with ART's arguments (patches/art/0800): it runs and returns.
  using LoadClassFn = uint64_t(SYSV*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
  auto load = reinterpret_cast<LoadClassFn>(arm64::HostEntry(load_class));
  const uint64_t loaded = load != nullptr ? load(0x10, 0x20, 0x30, 0x40, 0x50) : 1;
  Check("LoadClass runs under the translator and returns (X0 untouched)", loaded == 0x10,
        Hex(loaded));

  std::printf(g_failures == 0 ? "art_shim_arm64_test all passed\n"
                              : "art_shim_arm64_test: %d failed\n",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
