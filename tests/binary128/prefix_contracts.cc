// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "../../compat/bionic/binary128_parse.h"
#include "parse_cases.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pinned {
// The same string-backed FILE environment used by bionic/wcstod.cpp. No
// filesystem I/O is involved. This lets the pinned scanner run unchanged.
struct File { unsigned char* _p; int _r; };
int __srefill(File*) { return -1; }
int ungetc(int c, File* file) { --file->_p; ++file->_r; return c; }
#define FILE File
#include "pinned_parsefloat.inc"
#undef FILE
}  // namespace pinned

static int Check(const std::string& input) {
  std::string buffer = input;
  pinned::File file{reinterpret_cast<unsigned char*>(buffer.data()), static_cast<int>(buffer.size())};
  size_t expected = pinned::parsefloat(&file, buffer.data(), buffer.data() + buffer.size());
  size_t actual = gesher::bionic::Binary128WidePrefix(input.c_str());
  if (expected == actual) return 0;
  std::printf("FAIL prefix '%s': pinned %zu, production %zu\n", input.c_str(), expected, actual);
  return 1;
}

int main() {
  int failures = 0;
  unsigned checks = 0;
  for (const auto& sample : quad_cases) {
    failures += Check(sample.text);
    ++checks;
  }
  constexpr char alphabet[] = "-+0123456789.xXeEpP()nNaAiIfFtTyY_abcdef";
  uint32_t state = 0x5639;
  auto next = [&] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
  const char* prefixes[] = {"", "-", "0x", "-0X", ".", "1e", "0x1p", "nan(", "-NaN(", "inf", "INFINITY"};
  for (unsigned i = 0; i < 20000; ++i) {
    std::string text = prefixes[next() % (sizeof(prefixes) / sizeof(prefixes[0]))];
    unsigned length = next() % 40;
    for (unsigned j = 0; j < length; ++j) text += alphabet[next() % (sizeof(alphabet) - 1)];
    failures += Check(text);
    ++checks;
  }
  std::printf("Wide binary128 prefixes: %u pinned comparisons, %d failures\n", checks, failures);
  return failures != 0;
}
