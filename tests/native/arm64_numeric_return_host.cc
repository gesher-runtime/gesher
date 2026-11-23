// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Loads the ordinary NDK-built numeric fixture through the production ELF
// loader, Bionic shim and ARM64 translator. No replacement numeric providers.
#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <cfenv>
#include <cstdio>
#include <cstring>
#include <string>
#include <xmmintrin.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <arm64_numeric_return.so>\n", argv[0]);
    return 2;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  gesher::bionic::InstallBionicShim();
  if (!gesher::arm64::EnsureLoaded()) return 3;
  std::string error;
  auto* library = gesher::elf::Load(argv[1], &error);
  if (library == nullptr) {
    std::fprintf(stderr, "numeric guest load failed: %s\n", error.c_str());
    return 4;
  }
  void* entry = gesher::elf::Lookup(library, "gesher_numeric_selftest");
  if (entry == nullptr) return 5;
  std::fenv_t original;
  std::fegetenv(&original);
  const unsigned original_mxcsr = _mm_getcsr();
  unsigned failures = 0;
  const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  for (int mode : modes) {
    std::fesetenv(FE_DFL_ENV);
    std::fesetround(mode);
    std::feraiseexcept(FE_INVALID | FE_DIVBYZERO);
    std::fenv_t before{}, after{};
    std::fegetenv(&before);
    const unsigned before_mxcsr = _mm_getcsr();
    const uint64_t result = gesher::arm64::Call(entry, nullptr, 0);
    std::fegetenv(&after);
    const bool restored = std::memcmp(&before, &after, sizeof before) == 0 &&
                          _mm_getcsr() == before_mxcsr;
    if (result != 0 || !restored) ++failures;
    std::printf("NUMERIC_HOST mode=%d guest_result=%llu native_fenv_restored=%d\n",
                mode, static_cast<unsigned long long>(result), restored);
  }
  std::fesetenv(&original);
  _mm_setcsr(original_mxcsr);
  gesher::elf::Close(library);
  std::printf("NUMERIC_HOST_DONE runs=4 failures=%u\n", failures);
  return failures == 0 ? 0 : 1;
}
