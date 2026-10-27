// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include <gesher/bionic_shim.h>
#include <unicode/uchar.h>
#include <unicode/uversion.h>

#include <array>
#include <cinttypes>
#include <cstdio>
#include <string>

int main() {
  gesher::bionic::InstallBionicShim();
  using Plain = uint32_t(__attribute__((sysv_abi))*)(uint32_t);
  using Localized = uint32_t(__attribute__((sysv_abi))*)(uint32_t, void*);
  using NewLocale = void*(__attribute__((sysv_abi))*)(int, const char*, void*);
  using FreeLocale = void(__attribute__((sysv_abi))*)(void*);
  using Width = int(__attribute__((sysv_abi))*)(uint32_t);
  auto lookup = gesher::bionic::LookupBionicSymbol;
  auto make_locale = reinterpret_cast<NewLocale>(lookup("newlocale"));
  auto free_locale = reinterpret_cast<FreeLocale>(lookup("freelocale"));
  auto width = reinterpret_cast<Width>(lookup("wcwidth"));
  if (make_locale == nullptr || free_locale == nullptr || width == nullptr) return 1;
  void* locale = make_locale(1, "C.UTF-8", nullptr);
  if (locale == nullptr) return 1;
  const char* names[] = {"alnum", "alpha",  "blank",   "cntrl",  "digit",
                         "graph", "lower",  "print",   "punct",  "space",
                         "upper", "xdigit", "tolower", "toupper"};
  std::array<Plain, 14> plain;
  std::array<Localized, 14> localized;
  std::array<uint64_t, 14> plain_hash, locale_hash;
  plain_hash.fill(UINT64_C(0xcbf29ce484222325));
  locale_hash = plain_hash;
  uint64_t width_hash = UINT64_C(0xcbf29ce484222325);
  for (size_t i = 0; i < plain.size(); ++i) {
    std::string symbol = i < 12    ? std::string("isw") + names[i]
                         : i == 12 ? "towlower"
                                   : "towupper";
    plain[i] = reinterpret_cast<Plain>(lookup(symbol.c_str()));
    localized[i] = reinterpret_cast<Localized>(lookup((symbol + "_l").c_str()));
    if (plain[i] == nullptr || localized[i] == nullptr) {
      std::printf("FAIL  Unicode symbol %s or its locale form is absent\n",
                  symbol.c_str());
      free_locale(locale);
      return 1;
    }
  }
  UVersionInfo version;
  char version_text[U_MAX_VERSION_STRING_LENGTH];
  u_getUnicodeVersion(version);
  u_versionToString(version, version_text);
  std::printf("ICU4C Unicode data %s\n", version_text);
  auto add = [&](uint32_t c) {
    uint32_t columns = width(c);
    for (unsigned shift = 0; shift < 32; shift += 8) {
      width_hash =
          (width_hash ^ ((columns >> shift) & 0xff)) * UINT64_C(0x100000001b3);
    }
    for (size_t i = 0; i < plain.size(); ++i) {
      uint32_t a = plain[i](c), b = localized[i](c, locale);
      for (unsigned shift = 0; shift < 32; shift += 8) {
        plain_hash[i] =
            (plain_hash[i] ^ ((a >> shift) & 0xff)) * UINT64_C(0x100000001b3);
        locale_hash[i] =
            (locale_hash[i] ^ ((b >> shift) & 0xff)) * UINT64_C(0x100000001b3);
      }
    }
  };
  for (uint32_t c = 0; c <= 0x10ffff; ++c) add(c);
  for (uint32_t c : {UINT32_C(0x110000), UINT32_C(0x7fffffff),
                     UINT32_C(0x80000000), UINT32_C(0xffffffff)})
    add(c);
  for (size_t i = 0; i < plain.size(); ++i) {
    std::printf("HOST_UNICODE %s %016" PRIx64 "\n", names[i], plain_hash[i]);
    std::printf("HOST_UNICODE_L %s %016" PRIx64 "\n", names[i], locale_hash[i]);
  }
  free_locale(locale);
  std::printf("HOST_WIDTH wcwidth %016" PRIx64 "\n", width_hash);
  return 0;
}
