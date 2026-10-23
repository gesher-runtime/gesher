// Loads an x86-64 Android .so with gesher's ELF loader and exercises it.
//
//   elf_load_test <library.so> [search-dir ...]
//
// The bionic shim (compat/bionic) is installed first, so the library may
// import libc, libm, libdl, liblog and pthread.
//
// A library that exports gesher_selftest() runs its own checks: the test calls
// it and expects 0 failures (tests/native/libc_test.c). Otherwise it is the
// step-1 check (tests/native/trivial.c): the loader ran its constructors (the
// library exports gesher_ctor_ran()), gesher_answer() returns 42 and
// gesher_add(40, 2) returns 42, which exercises mapping, relocations
// (RELATIVE/RELR) and init arrays on a self-contained object.
//
// Prints "elf_load_test all passed" and exits 0 on success.

#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace elf = gesher::elf;

// Loaded code is System V ABI (docs/decisions/0006-managed-abi.md), so its
// entry points are called through sysv_abi pointers.
#define SYSV __attribute__((sysv_abi))

static int failures = 0;

static void Check(const char* what, bool ok, const char* detail = "") {
  std::printf("%-44s %s%s%s\n", what, ok ? "ok" : "FAIL", *detail ? "  " : "", detail);
  if (!ok) ++failures;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <library.so> [search-dir ...]\n", argv[0]);
    return 2;
  }

  gesher::bionic::InstallBionicShim();

  // %fs for this thread, so any thread-local or stack-protector code works.
  bool tls = elf::SetupThreadTls();
  std::printf("thread TLS (%%fs) set up: %s\n", tls ? "yes" : "no (host lacks WRFSBASE)");

  elf::LoadOptions options;
  for (int i = 2; i < argc; ++i) options.search_paths.emplace_back(argv[i]);

  std::string error;
  elf::Library* lib = elf::Load(argv[1], &error, options);
  Check("library loaded", lib != nullptr, error.c_str());
  if (lib == nullptr) {
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
  }

  // A library with its own checks.
  if (auto selftest = reinterpret_cast<int (SYSV*)()>(elf::Lookup(lib, "gesher_selftest"))) {
    int n = selftest();
    std::fflush(stdout);
    Check("gesher_selftest() reports no failures", n == 0);
    if (failures == 0) {
      std::printf("\nelf_load_test all passed\n");
    } else {
      std::printf("\n%d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
  }

  auto ctor_ran = reinterpret_cast<int (SYSV*)()>(elf::Lookup(lib, "gesher_ctor_ran"));
  Check("gesher_ctor_ran found", ctor_ran != nullptr);
  if (ctor_ran) Check("constructors ran before Lookup", ctor_ran() == 1);

  auto answer = reinterpret_cast<int (SYSV*)()>(elf::Lookup(lib, "gesher_answer"));
  Check("gesher_answer found", answer != nullptr);
  if (answer) Check("gesher_answer() == 42", answer() == 42);

  auto add = reinterpret_cast<int (SYSV*)(int, int)>(elf::Lookup(lib, "gesher_add"));
  Check("gesher_add found", add != nullptr);
  if (add) Check("gesher_add(40, 2) == 42", add(40, 2) == 42);

  // A missing symbol returns null, not a crash.
  Check("missing symbol returns null", elf::Lookup(lib, "gesher_nonexistent") == nullptr);

  if (failures == 0) {
    std::printf("\nelf_load_test all passed\n");
  } else {
    std::printf("\n%d check(s) failed\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
