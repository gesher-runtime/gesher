// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <gesher/bionic_shim.h>
#include <cstdio>

#if defined(__x86_64__)
#define BINARY128_CALL __attribute__((sysv_abi))
#endif
#include "../binary128/public_contracts.h"

int main() {
  gesher::bionic::InstallBionicShim();
  using Quad = int32_t __attribute__((vector_size(16)));
  binary128_contract::Api<Quad, uint32_t, void*> api;
  unsigned checks = 0, failures = 0;
  auto check = [&](bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL binary128: %s\n", what.c_str()); }
  };
  if (!api.Load(gesher::bionic::LookupBionicSymbol, check)) return 1;
#if defined(__x86_64__)
  const std::array<int, 4> modes{0xc00, 0, 0x800, 0x400};
  const std::array<int, 5> flags{1, 4, 8, 16, 32};
#else
  const std::array<int, 4> modes{0xc00000, 0, 0x400000, 0x800000};
  const std::array<int, 5> flags{1, 2, 4, 8, 16};
#endif
  binary128_contract::Run(api, check, "production System V entries", modes, flags, 1);

  // Source-grounded native control: gdtoa's initial approximation of 0.1
  // performs inexact binary64 arithmetic. Unlike the Android emulator used
  // for value observations, an actual Windows CPU must expose that flag.
  alignas(16) unsigned char saved[32]{};
  check(api.get_env(saved) == 0, "save native inexact control environment");
  for (int mode : modes) {
    check(api.set_round(mode) == 0, "native inexact control rounding");
    check(api.clear_flags(0x3f) == 0, "native inexact control clear");
    Quad result = api.narrow("0.1", nullptr);
    (void)result;
    check((api.get_flags(flags[4]) & flags[4]) != 0, "native conversion exposes real raised inexact");
  }
  check(api.set_env(saved) == 0, "restore native inexact control environment");
  std::printf("Binary128 production parsing: %u checks, %u failures\n", checks, failures);
  return failures != 0;
}
