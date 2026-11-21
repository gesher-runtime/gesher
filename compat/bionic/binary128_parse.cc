// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "binary128_parse.h"

#include <bit>
#include <cfloat>
#include <cfenv>
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

extern "C" int gesher_gdtoa_strtorQ(const char*, char**, int, void*);

namespace gesher::bionic {

static_assert(std::endian::native == std::endian::little && CHAR_BIT == 8);
static_assert(sizeof(int) == 4 && sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024);

int Binary128RoundingMode() {
  switch (std::fegetround()) {
    case FE_TOWARDZERO: return 0;
    case FE_UPWARD: return 2;
    case FE_DOWNWARD: return 3;
    default: return 1;
  }
}

void ParseBinary128(const char* text, char** end, int rounding, void* output) {
  // Upstream shares its bigint free list, powers-of-five cache and hex digit
  // table. Protect their complete lifetime, including lazy initialization.
  // Only this entry point calls the private, prefixed gdtoa functions.
  static std::mutex parser_mutex;
  std::lock_guard lock(parser_mutex);
  uint32_t words[4];
  gesher_gdtoa_strtorQ(text, end, rounding, words);
  std::memcpy(output, words, sizeof(words));
}

size_t Binary128WidePrefix(const char* text) {
  // This is the committed-prefix grammar of pinned bionic/stdio/parsefloat.c,
  // used by bionic/wcstod.cpp before the actual numeric conversion.
  std::string_view s(text);
  size_t pos = 0;
  if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
  auto matches = [&](std::string_view word) {
    if (s.size() - pos < word.size()) return false;
    for (size_t i = 0; i < word.size(); ++i) {
      char c = s[pos + i];
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      if (c != word[i]) return false;
    }
    return true;
  };
  if (matches("inf")) return pos + (matches("infinity") ? 8 : 3);
  if (matches("nan")) {
    size_t committed = pos + 3;
    pos = committed;
    if (pos == s.size() || s[pos++] != '(') return committed;
    while (pos < s.size()) {
      char c = s[pos++];
      if (c == ')') return pos;
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '_')) break;
    }
    return committed;
  }
  size_t committed = 0;
  bool hex = s.size() - pos >= 2 && s[pos] == '0' &&
             (s[pos + 1] == 'x' || s[pos + 1] == 'X');
  if (hex) {
    committed = pos + 1;  // An incomplete 0x token still commits its zero.
    pos += 2;
  }
  auto digit = [&](char c) {
    return (c >= '0' && c <= '9') ||
           (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
  };
  bool any = false;
  while (pos < s.size() && digit(s[pos])) {
    any = true;
    committed = ++pos;
  }
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    if (any) committed = pos;
    while (pos < s.size() && digit(s[pos])) {
      any = true;
      committed = ++pos;
    }
  }
  if (any && pos < s.size() &&
      (hex ? (s[pos] == 'p' || s[pos] == 'P') : (s[pos] == 'e' || s[pos] == 'E'))) {
    ++pos;
    if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') committed = ++pos;
  }
  return committed;
}

}  // namespace gesher::bionic
