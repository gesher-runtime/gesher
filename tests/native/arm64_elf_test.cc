// Loads an arm64-v8a Android .so with gesher's ELF loader and the arm64
// translator (gesher_arm64.dll) and exercises it (Phase 7, docs/arm64.md).
//
//   arm64_elf_test <library.so> [search-dir ...]
//
// The bionic shim (compat/bionic) is installed first, so the guest library may
// import libc, libm, libdl, liblog and pthread; its imports are bound to guest
// trampolines that call the host shim across the AAPCS64 <-> System V boundary.
// Guest exports are entered through host_entry() stubs.
//
// A library that exports gesher_selftest() runs its own checks and must return
// 0 failures (tests/native/libc_test.c). Otherwise it is the step-1 trivial one
// (tests/native/trivial.c): gesher_answer() == 42, gesher_add(40,2) == 42, and
// the constructor ran (gesher_ctor_ran()).
//
// Prints "arm64_elf_test all passed" and exits 0 on success.

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace elf = gesher::elf;
namespace arm64 = gesher::arm64;

static int failures = 0;
static void Check(const char* what, bool ok, const char* detail = "") {
  std::printf("%-52s %s%s%s\n", what, ok ? "ok" : "FAIL", *detail ? "  " : "", detail);
  if (!ok) ++failures;
}

// A guest export, entered through a host_entry System V stub.
template <typename Fn>
static Fn Entry(elf::Library* lib, const char* name) {
  void* guest = elf::Lookup(lib, name);
  if (guest == nullptr) return nullptr;
  return reinterpret_cast<Fn>(arm64::HostEntry(guest));
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <library.so> [search-dir ...]\n", argv[0]);
    return 2;
  }

  gesher::bionic::InstallBionicShim();
  Check("arm64 translator available", arm64::EnsureLoaded());

  elf::LoadOptions options;
  for (int i = 2; i < argc; ++i) options.search_paths.emplace_back(argv[i]);

  std::string error;
  elf::Library* lib = elf::Load(argv[1], &error, options);
  Check("guest library loaded", lib != nullptr, error.c_str());
  if (lib == nullptr) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }

  using IntFn = int(__attribute__((sysv_abi))*)();
  using AddFn = int(__attribute__((sysv_abi))*)(int, int);

  if (auto selftest = Entry<IntFn>(lib, "gesher_selftest")) {
    int rc = selftest();
    Check("gesher_selftest() == 0", rc == 0);
  } else {
    auto ctor = Entry<IntFn>(lib, "gesher_ctor_ran");
    auto answer = Entry<IntFn>(lib, "gesher_answer");
    auto add = Entry<AddFn>(lib, "gesher_add");
    Check("exports found", ctor && answer && add);
    if (ctor && answer && add) {
      Check("constructor ran", ctor() == 1);
      Check("gesher_answer() == 42", answer() == 42);
      Check("gesher_add(40, 2) == 42", add(40, 2) == 42);
    }
  }

  std::printf("\n");
  if (failures == 0) {
    std::printf("arm64_elf_test all passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", failures);
  return 1;
}
