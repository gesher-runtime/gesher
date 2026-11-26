// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Shared by the actual-source host probe and Windows Bionic ABI fixture.
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gesher_snprintf_contract {
template <class Print, class Check, class... Args>
void Expected(Print print, Check check, const std::string& expected,
              const char* format, Args... arguments) {
  const size_t capacities[] = {0, 1, 2, 7, 15, 16, 17, 31, 64, 127, 128, 129,
                               255, 256, 257, 4096};
  for (size_t capacity : capacities) {
    std::vector<unsigned char> actual(capacity + 32, 0xa5);
    auto* destination = reinterpret_cast<char*>(actual.data() + 16);
    int result = print(destination, capacity, format, arguments...);
    check("snprintf complete count through truncation", result == static_cast<int>(expected.size()));
    size_t kept = capacity ? (expected.size() < capacity - 1 ? expected.size() : capacity - 1) : 0;
    check("snprintf exact retained bytes", std::memcmp(destination, expected.data(), kept) == 0);
    check("snprintf terminator", capacity == 0 || destination[kept] == '\0');
    for (size_t i = 0; i < actual.size(); ++i) {
      bool written = capacity != 0 && i >= 16 && i <= 16 + kept;
      check("snprintf surrounding bytes untouched", written || actual[i] == 0xa5);
    }
  }
  check("snprintf null zero-size destination", print(nullptr, 0, format, arguments...) ==
        static_cast<int>(expected.size()));
  std::vector<char> full(expected.size() + 2, '\x5a');
  int result = print(full.data(), expected.size() + 1, format, arguments...);
  check("snprintf exact full-size output", result == static_cast<int>(expected.size()) &&
        std::memcmp(full.data(), expected.data(), expected.size()) == 0 &&
        full[expected.size()] == '\0' && full.back() == '\x5a');
}

template <class Print, class Check, class... Args>
void Common(Print print, Check check, const char* format, Args... arguments) {
  int count = std::snprintf(nullptr, 0, format, arguments...);
  check("reference formatting succeeded", count >= 0);
  if (count < 0) return;
  std::vector<char> reference(static_cast<size_t>(count) + 1);
  int second = std::snprintf(reference.data(), reference.size(), format, arguments...);
  check("reference complete count stable", second == count);
  Expected(print, check, std::string(reference.data(), static_cast<size_t>(count)),
           format, arguments...);
}

template <class Print, class Check>
void Run(Print print, Check check) {
  Expected(print, check, "", "");
  Expected(print, check, "literal % % trailing", "literal %% %% trailing");
  Common(print, check, "%d:%s:%08x", -214, "payload", 0xfeed1234u);
  Common(print, check, "%hhd/%hhu/%hd/%hu", 255, 511u, 65535, 131071u);
  Common(print, check, "%lld/%llu/%#llx/%llo", LLONG_MIN, ULLONG_MAX,
         0xfedcba9876543210ull, 0123456701234567ull);
  Common(print, check, "%jd/%ju/%zu/%td", static_cast<intmax_t>(-987654321234ll),
         static_cast<uintmax_t>(987654321234ull), static_cast<size_t>(123456),
         static_cast<ptrdiff_t>(-123456));
  Common(print, check, "%+020d|%#016x|%-18.7s|%.*s|%*d", -1234567,
         0x1234u, "abcdefghijk", -5, "negative precision", -23, 42);
  Common(print, check, "%cA%cB%c", 0, 'Q', 0);
  Common(print, check, "%s|%.0s|%.3s", "\xc3\xa9\xe2\x82\xac", "discarded", "abcdef");
  Common(print, check, "%d/%d/%d/%d/%d/%d/%d/%d/%d/%d", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  Common(print, check, "%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f/%.2f",
         0.5, -1.5, 2.25, 3.5, -4.75, 5.125, 6.5, 7.75, 8.25, -9.5);
  for (int i = -12; i <= 12; ++i)
    Common(print, check, "%+*.*f|%#.*g|%.5a|%.*s", i * 3, (i + 12) % 9,
           i * 1.125, (i + 12) % 8 + 1, i * 123.5, i * 0.125,
           (i + 12) % 16, "0123456789abcdef");
  std::string large(65537, 'x'); large[0] = 'A'; large.back() = 'Z';
  Common(print, check, "prefix:%s:suffix:%d", large.c_str(), 928);
  Common(print, check, "%131073s|%d", "end", 17);
  Common(print, check, "%.*s:%*.*d", 32771, large.c_str(), -65537, 5, 123);
  Expected(print, check, large, large.c_str());
  Expected(print, check, "0x0/0x123ab", "%p/%p", static_cast<void*>(nullptr),
           reinterpret_cast<void*>(uintptr_t{0x123ab}));
  const uint32_t wide[] = {0x3c0, 0x1f642, 'Z', 0};
  Expected(print, check, "\xcf\x80\xf0\x9f\x99\x82Z", "%ls", wide);
  Expected(print, check, "\xcf\x80\xf0\x9f\x99\x82", "%.2ls", wide);
  Expected(print, check, "\xcf\x80", "%lc", static_cast<uint32_t>(0x3c0));
  Expected(print, check, "(null)", "%s", static_cast<const char*>(nullptr));
  Expected(print, check, "unsupported:%2$d/end", "unsupported:%2$d/end", 1, 2);
  Expected(print, check, "trailing:%", "trailing:%");
  char unchanged[48]; std::memset(unchanged, 0x5a, sizeof(unchanged));
  int result = print(unchanged, sizeof(unchanged), "prefix:%s:%*d", "value", INT_MIN, 7);
  check("invalid late width remains failure", result == -1);
  for (char value : unchanged) check("invalid format leaves destination intact", value == '\x5a');
  result = print(unchanged, sizeof(unchanged), "prefix:%999999999999999999999d", 7);
  check("overflowed width remains failure", result == -1);
  for (char value : unchanged) check("overflowed format leaves destination intact", value == '\x5a');
}
template <class Print, class Check, class Error, class Describe>
void Errors(Print print, Check check, Error error, Describe describe) {
  std::string message = describe(2);
  std::string expected = "errno:" + message + "|" + std::string(131072, ' ') +
                         "Q|1.25|" + message;
  for (size_t capacity : {size_t{0}, size_t{1}, size_t{8}, size_t{129}, size_t{4096}}) {
    std::vector<char> output(capacity + 16, '\x5a');
    *error() = 2;
    int result = print(output.data(), capacity, "errno:%m|%131073s|%.2f|%m", "Q", 1.25);
    int final_error = *error();
    check("successful truncated conversions preserve entry errno", final_error == 2);
    check("percent-m retains original errno messages", result == static_cast<int>(expected.size()));
    size_t retained = capacity ? capacity - 1 : 0;
    check("percent-m exact prefix", std::memcmp(output.data(), expected.data(), retained) == 0);
    check("percent-m terminator", !capacity || output[retained] == 0);
    for (size_t i = capacity; i < output.size(); ++i)
      check("percent-m untouched suffix", output[i] == '\x5a');
  }
  for (size_t capacity : {size_t{0}, size_t{1}, size_t{48}}) {
    char output[48]; std::memset(output, 0x5a, sizeof(output));
    *error() = 2;
    int result = print(output, capacity, "prefix:%s:%.999999999999999999999d", "value", 7);
    int final_error = *error();
    check("late precision overflow retains failure errno", result == -1 && final_error == 12);
    for (char value : output)
      check("late precision overflow preserves destination", value == '\x5a');
  }
}
}  // namespace gesher_snprintf_contract
