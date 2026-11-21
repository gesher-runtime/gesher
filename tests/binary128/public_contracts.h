// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "parse_cases.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef BINARY128_CALL
#define BINARY128_CALL
#endif

namespace binary128_contract {

struct Expected {
  uint64_t low, high;
  int consumed, error, flags;
};

// Captured from the ordinary Android reference program, not this backend.
#include "android_expected.inc"

template <class Value, class Wide, class Locale>
struct Api {
  using Quad = Value;
  using Wchar = Wide;
  Value (BINARY128_CALL *narrow)(const char*, char**) = nullptr;
  Value (BINARY128_CALL *narrow_l)(const char*, char**, Locale) = nullptr;
  Value (BINARY128_CALL *wide)(const Wide*, Wide**) = nullptr;
  Value (BINARY128_CALL *wide_l)(const Wide*, Wide**, Locale) = nullptr;
  Locale (BINARY128_CALL *make_locale)(int, const char*, Locale) = nullptr;
  void (BINARY128_CALL *free_locale)(Locale) = nullptr;
  int* (BINARY128_CALL *error)() = nullptr;
  int (BINARY128_CALL *set_round)(int) = nullptr;
  int (BINARY128_CALL *get_round)() = nullptr;
  int (BINARY128_CALL *clear_flags)(int) = nullptr;
  int (BINARY128_CALL *raise_flags)(int) = nullptr;
  int (BINARY128_CALL *get_flags)(int) = nullptr;
  int (BINARY128_CALL *get_env)(void*) = nullptr;
  int (BINARY128_CALL *set_env)(const void*) = nullptr;

  template <class Lookup, class Check>
  bool Load(Lookup lookup, Check check) {
    bool complete = true;
#define QUAD_LOAD(field, name) \
    field = reinterpret_cast<decltype(field)>(lookup(name)); \
    check(field != nullptr, std::string("binary128 symbol ") + name); \
    complete &= field != nullptr
    QUAD_LOAD(narrow, "strtold");
    QUAD_LOAD(narrow_l, "strtold_l");
    QUAD_LOAD(wide, "wcstold");
    QUAD_LOAD(wide_l, "wcstold_l");
    QUAD_LOAD(make_locale, "newlocale");
    QUAD_LOAD(free_locale, "freelocale");
    QUAD_LOAD(error, "__errno");
    QUAD_LOAD(set_round, "fesetround");
    QUAD_LOAD(get_round, "fegetround");
    QUAD_LOAD(clear_flags, "feclearexcept");
    QUAD_LOAD(raise_flags, "feraiseexcept");
    QUAD_LOAD(get_flags, "fetestexcept");
    QUAD_LOAD(get_env, "fegetenv");
    QUAD_LOAD(set_env, "fesetenv");
#undef QUAD_LOAD
    return complete;
  }
};

template <class Api, class Check>
void Run(Api& api, Check check, const char* route, const std::array<int, 4>& modes,
         const std::array<int, 5>& flag_bits, int locale_mask) {
  static_assert(sizeof(typename Api::Quad) == 16 && sizeof(typename Api::Wchar) == 4);
  int all_flags = 0;
  for (int flag : flag_bits) all_flags |= flag;
  auto flags = [&] {
    int actual = api.get_flags(all_flags), normalized = 0;
    for (size_t i = 0; i < flag_bits.size(); ++i) if (actual & flag_bits[i]) normalized |= 1 << i;
    return normalized;
  };
  alignas(16) unsigned char saved_environment[32]{};
  int saved_errno = *api.error();
  check(api.get_env(saved_environment) == 0, std::string(route) + " save fenv");
  for (const char* locale_name : {"C", "C.UTF-8"}) {
    auto locale = api.make_locale(locale_mask, locale_name, nullptr);
    check(locale != nullptr, std::string(route) + " locale " + locale_name);
    if (locale == nullptr) continue;
    for (size_t input = 0; input < std::size(quad_cases); ++input) {
      const auto& sample = quad_cases[input];
      std::vector<typename Api::Wchar> wide;
      if (sample.wide_space) wide.push_back(0x2003);
      for (const unsigned char* p = reinterpret_cast<const unsigned char*>(sample.text); *p; ++p)
        wide.push_back(*p);
      wide.push_back(0);
      for (unsigned mode = 0; mode < 4; ++mode) {
        for (unsigned form = 0; form < 4; ++form) {
          const auto& wanted = android_expected[input][mode][form / 2];
          for (int sticky : {0, 1}) {
            std::string tag = std::string(route) + " " + locale_name + " " + sample.name +
                              " mode=" + std::to_string(mode) + " api=" + std::to_string(form) +
                              " sticky=" + std::to_string(sticky);
            check(api.set_round(modes[mode]) == 0, tag + " select rounding");
            check(api.clear_flags(all_flags) == 0, tag + " clear flags");
            if (sticky) check(api.raise_flags(flag_bits[1] | flag_bits[4]) == 0, tag + " seed sticky flags");
            *api.error() = 123;
            char* end = nullptr;
            typename Api::Wchar* wend = nullptr;
            typename Api::Quad value;
            switch (form) {
              case 0: value = api.narrow(sample.text, &end); break;
              case 1: value = api.narrow_l(sample.text, &end, locale); break;
              case 2: value = api.wide(wide.data(), &wend); break;
              default: value = api.wide_l(wide.data(), &wend, locale); break;
            }
            int error = *api.error();
            int exceptions = flags();
            int actual_round = api.get_round();
            uint64_t bits[2];
            std::memcpy(bits, &value, sizeof(bits));
            ptrdiff_t consumed = form < 2 ? end - sample.text : wend - wide.data();
            check(bits[0] == wanted.low && bits[1] == wanted.high, tag + " all 128 bits");
            check(consumed == wanted.consumed, tag + " end pointer");
            check(error == wanted.error, tag + " errno");
            // The Android emulator cannot observe arithmetic-raised inexact
            // (see its separate volatile-division control). Do not turn zero
            // in that receipt into a requirement to erase real host flags.
            // The integer/range paths do not raise the other four exceptions;
            // explicitly raised DIVBYZERO and INEXACT must remain sticky.
            check((exceptions & ~16) == (wanted.flags | (sticky ? 2 : 0)), tag + " fenv non-inexact flags");
            if (sticky) check((exceptions & 18) == 18, tag + " preserve seeded sticky flags");
            check(actual_round == modes[mode], tag + " rounding unchanged");
          }
        }
      }
    }
    check(api.set_round(modes[1]) == 0, std::string(route) + " null-end rounding");
    const char* text = "0x1.0000000000000000000000000001p0";
    std::vector<typename Api::Wchar> wide;
    for (const char* p = text; *p; ++p) wide.push_back(*p);
    wide.push_back(0);
    const typename Api::Quad results[] = {api.narrow(text, nullptr), api.narrow_l(text, nullptr, locale),
                                         api.wide(wide.data(), nullptr), api.wide_l(wide.data(), nullptr, locale)};
    for (const auto& value : results) {
      uint64_t bits[2];
      std::memcpy(bits, &value, sizeof(bits));
      check(bits[0] == 1 && bits[1] == UINT64_C(0x3fff000000000000), std::string(route) + " null end pointer");
    }
    api.free_locale(locale);
  }
  check(api.set_env(saved_environment) == 0, std::string(route) + " restore fenv");
  *api.error() = saved_errno;
}

}  // namespace binary128_contract
