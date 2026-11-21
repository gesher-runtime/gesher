// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/bionic/binary128_parse.h"

#include <cerrno>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

static int Flags() {
  int f = std::fetestexcept(FE_ALL_EXCEPT);
  return ((f & FE_INVALID) ? 1 : 0) | ((f & FE_DIVBYZERO) ? 2 : 0) |
         ((f & FE_OVERFLOW) ? 4 : 0) | ((f & FE_UNDERFLOW) ? 8 : 0) |
         ((f & FE_INEXACT) ? 16 : 0);
}

int main() {
  const int modes[] = {FE_TOWARDZERO, FE_TONEAREST, FE_UPWARD, FE_DOWNWARD};
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.size() < 2 || line[0] < '0' || line[0] > '3' || line[1] != '\t') return 2;
    int mode = line[0] - '0';
    const char* text = line.c_str() + 2;
    if (std::fesetround(modes[mode]) != 0 || std::feclearexcept(FE_ALL_EXCEPT) != 0) return 3;
    errno = 123;
    uint64_t output[2];
    char* end = nullptr;
    int observed_mode = gesher::bionic::Binary128RoundingMode();
    gesher::bionic::ParseBinary128(text, &end, observed_mode, output);
    int error = errno;
    int flags = Flags();
    int unchanged = std::fegetround() == modes[mode] && observed_mode == mode;
    std::printf("%016llx%016llx %td %d %d %d\n",
                static_cast<unsigned long long>(output[1]),
                static_cast<unsigned long long>(output[0]), end - text, error, flags, unchanged);
  }
  return 0;
}
