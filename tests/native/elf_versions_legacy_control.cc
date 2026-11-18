// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Historical-source behavioral control using only the pre-versioning API.
#include <gesher/elf_loader.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace elf = gesher::elf;
static void Event(int) {}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  elf::SetBionicResolver([](const char* name) -> void* {
    return std::strcmp(name, "gv_event") == 0 ? reinterpret_cast<void*>(&Event) : nullptr;
  });
  elf::LoadOptions options;
  options.allow_shimmed_needed = true;  // Historical callback-only test boundary.
  options.stub_unresolved_functions = false;
  std::string error;
  const std::string path = std::string(argv[1]) + "/lookup-gnu/libgv_provider.so";
  auto* library = elf::Load(path.c_str(), &error, options);
  if (!library) {
    std::fprintf(stderr, "FAIL historical valid provider load: %s\n", error.c_str());
    return 2;
  }
  if (elf::Lookup(library, "gv_hidden") != nullptr) {
    std::fputs("FAIL version contract: ordinary Lookup exposed a hidden-only version\n", stderr);
    return 1;
  }
  std::puts("PASS version contract: ordinary Lookup excludes a hidden-only version");
  return 0;
}
