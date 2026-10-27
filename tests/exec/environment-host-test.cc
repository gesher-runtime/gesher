// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Host mutations must reach an already-relocated Android ELF environ import.
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <cstdint>
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
  // Retain only the pointer's integer value: a negative control must never
  // traverse the former allocation after the CRT has changed its storage.
  const uintptr_t snapshot = reinterpret_cast<uintptr_t>(_environ);
  std::string error;
  elf::Library* library = elf::Load(argv[1], &error);
  if (library == nullptr) {
    std::fprintf(stderr, "exec4 environment load failed: %s\n", error.c_str());
    return 1;
  }
  using Run = int(__attribute__((sysv_abi))*)(const char*, const char*);
  auto run = reinterpret_cast<Run>(elf::Lookup(library, "Exec4Environment"));
  auto mutate = reinterpret_cast<Run>(elf::Lookup(library, "Exec4EnvironmentMutations"));
  if (run == nullptr || mutate == nullptr) return 1;
  bool relocated = false;
  for (int i = 0; i < 64; ++i) {
    const std::string name = "GESHER_EXEC4_GROW_" + std::to_string(i);
    if (_putenv_s(name.c_str(), "host-narrow") != 0) return 1;
    relocated |= snapshot != reinterpret_cast<uintptr_t>(_environ);
    // Compare pointer values before allowing any guest code to dereference
    // its imported array. The old snapshot fails safely here.
    if (slot != &_environ || alias != slot || *slot != _environ) {
      std::fputs("exec4: FAIL environ does not export the current CRT slot\n", stderr);
      return 1;
    }
  }
  if (!relocated) {
    std::fputs("exec4: FAIL captured CRT snapshot did not become stale\n", stderr);
    return 1;
  }
  std::puts("exec4: PASS stale CRT snapshot detected without dereferencing it");
  if (_putenv_s("GESHER_EXEC4_ENV", "host-narrow") != 0 || run(argv[2], "host-narrow") != 0)
    return 1;
  if (_wputenv_s(L"GESHER_EXEC4_ENV", L"host-wide") != 0 ||
      _wputenv_s(L"GESHER_EXEC4_WIDE_ADDED", L"wide-added") != 0)
    return 1;
  if (slot != &_environ || *slot != _environ || run(argv[2], "host-wide") != 0) return 1;
  if (_wputenv_s(L"GESHER_EXEC4_ENV", L"") != 0 || *slot != _environ || run(argv[2], nullptr) != 0)
    return 1;
  if (mutate(argv[2], nullptr) != 0) return 1;
  std::puts(
      "exec4: PASS host narrow/wide changes reach relocated ELF imports "
      "and spawned children");
  return 0;
}
