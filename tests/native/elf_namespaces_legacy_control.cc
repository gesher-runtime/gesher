// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Old public API: different valid files must not alias merely by SONAME.
#include <gesher/elf_loader.h>
#include <cstdio>
#include <cstring>
#include <string>
#if defined(__x86_64__)
#define NS_SYSV __attribute__((sysv_abi))
#else
#define NS_SYSV
#endif
void NS_SYSV Event(int) {}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  namespace elf = gesher::elf;
  elf::SetBionicResolver([](const char* name) -> void* {
    return std::strcmp(name, "ns_event") == 0 ? reinterpret_cast<void*>(&Event) : nullptr;
  }, [](const char* name) { return std::strcmp(name, "libns_test_host.so") == 0; });
  elf::LoadOptions options; options.stub_unresolved_functions = false;
  std::string root = argv[1], error;
  auto* first = elf::Load((root + "/identity/a/libns_identity.so").c_str(), &error, options);
  if (!first) { std::fprintf(stderr, "control precondition failed: %s\n", error.c_str()); return 2; }
  auto* second = elf::Load((root + "/identity/b/libns_identity.so").c_str(), &error, options);
  if (!second) { std::fprintf(stderr, "control second load failed: %s\n", error.c_str()); return 2; }
  if (first == second) { std::puts("FAIL valid distinct same-SONAME files share one old handle"); return 1; }
  std::puts("PASS valid distinct same-SONAME files keep separate identities");
  return 0;
}
