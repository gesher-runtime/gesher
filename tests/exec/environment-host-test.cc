// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Host mutations must reach an already-relocated Android ELF environ import.
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>
#include <gesher/environment.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  namespace elf = gesher::elf;
  gesher::bionic::InstallBionicShim();
  if (!elf::SetupThreadTls()) return 1;
  auto slot = static_cast<char***>(gesher::bionic::LookupBionicSymbol("environ"));
  auto alias = static_cast<char***>(gesher::bionic::LookupBionicSymbol("__environ"));
  if (slot == nullptr) return 1;
  std::string error;
  elf::Library* library = elf::Load(argv[1], &error);
  if (library == nullptr) {
    std::fprintf(stderr, "exec4 environment load failed: %s\n", error.c_str());
    return 1;
  }
#if defined(__x86_64__)
#define SYSV_FN __attribute__((sysv_abi))
#elif defined(__aarch64__)
#define SYSV_FN
#endif
  using Run = int(SYSV_FN*)(const char*, const char*);
  auto run = reinterpret_cast<Run>(elf::Lookup(library, "Exec4Environment"));
  auto mutate = reinterpret_cast<Run>(elf::Lookup(library, "Exec4EnvironmentMutations"));
  if (run == nullptr || mutate == nullptr) return 1;
  auto check_guest = [&](Run test, const char* expected, const char* stage) {
    int result = test(argv[2], expected);
    if (result != 0) std::fprintf(stderr, "exec4: FAIL %s guest result %d\n", stage, result);
    return result == 0;
  };
  for (int i = 0; i < 64; ++i) {
    const std::string name = "GESHER_EXEC4_GROW_" + std::to_string(i);
    if (_putenv_s(name.c_str(), "host-narrow") != 0) return 1;
    // A copied pointer has a different slot even when the owner grows its
    // array in place. Verify the actual shared POSIX slot before traversal.
    if (slot != gesher::EnvironmentSlot() || alias != slot || *slot != environ) {
      std::fputs("exec4: FAIL environ does not export the current POSIX owner slot\n", stderr);
      return 1;
    }
  }
  std::puts("exec4: PASS live POSIX owner slot and aliases verified without dereferencing a snapshot");
  if (_putenv_s("GESHER_EXEC4_ENV", "host-narrow") != 0 ||
      !check_guest(run, "host-narrow", "host-narrow"))
    return 1;
  if (_wputenv_s(L"GESHER_EXEC4_ENV", L"host-wide") != 0 ||
      _wputenv_s(L"GESHER_EXEC4_WIDE_ADDED", L"wide-added") != 0)
    return 1;
  if (slot != gesher::EnvironmentSlot() || *slot != environ || !check_guest(run, "host-wide", "host-wide"))
    return 1;
  if (_wputenv_s(L"GESHER_EXEC4_ENV", L"") != 0 || *slot != environ ||
      !check_guest(run, nullptr, "host-wide removal"))
    return 1;
  if (!check_guest(mutate, nullptr, "guest mutations")) return 1;
  std::puts(
      "exec4: PASS host narrow/wide changes reach relocated ELF imports "
      "and spawned children");
  return 0;
}
