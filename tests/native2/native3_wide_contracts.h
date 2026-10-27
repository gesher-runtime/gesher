// Shared assertions run against unmodified pinned bionic in the independent
// Linux CI reference, and the actual shim entries in both NDK ABIs/modes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <iterator>
#include <algorithm>
#include <cstring>
#include <string>

template <class StateT>
struct WideApi {
  using State = StateT;
  size_t (*decode)(wchar_t*, const char*, size_t, State*);
  size_t (*decode32)(char32_t*, const char*, size_t, State*);
  size_t (*decode16)(char16_t*, const char*, size_t, State*);
  size_t (*encode)(char*, wchar_t, State*);
  size_t (*encode32)(char*, char32_t, State*);
  size_t (*encode16)(char*, char16_t, State*);
  size_t (*length)(const char*, size_t, State*);
  int (*initial)(const State*);
  int (*legacy_decode)(wchar_t*, const char*, size_t);
  int (*legacy_length)(const char*, size_t);
  int (*legacy_encode)(char*, wchar_t);
  size_t (*decode_string)(wchar_t*, const char**, size_t, State*);
  size_t (*decode_bounded)(wchar_t*, const char**, size_t, size_t, State*);
  size_t (*encode_string)(char*, const wchar_t**, size_t, State*);
  size_t (*encode_bounded)(char*, const wchar_t**, size_t, size_t, State*);
  size_t (*legacy_decode_string)(wchar_t*, const char*, size_t);
  size_t (*legacy_encode_string)(char*, const wchar_t*, size_t);
  long (*integer)(const wchar_t*, wchar_t**, int);
  unsigned long (*unsigned_integer)(const wchar_t*, wchar_t**, int);
};

template <class Api, class Check>
void WideContracts(const Api& api, Check check) {
  using State = typename Api::State;
  static_assert(sizeof(wchar_t) == 4 && sizeof(long) == 8 && sizeof(State) == 8);
  constexpr size_t bad = static_cast<size_t>(-1);
  constexpr size_t partial = static_cast<size_t>(-2);
  constexpr size_t pending = static_cast<size_t>(-3);
  auto initialized = [&] {
    State state{};
    std::memset(reinterpret_cast<unsigned char*>(&state) + 4, 0xa5, 4);
    return state;
  };
  auto reserved = [&](const State& state) {
    const auto* p = reinterpret_cast<const unsigned char*>(&state);
    return p[4] == 0xa5 && p[5] == 0xa5 && p[6] == 0xa5 && p[7] == 0xa5;
  };
  const struct { wchar_t code; const char* bytes; size_t size; } valid[] = {
      {0, "", 1}, {0x7f, "\x7f", 1}, {0x80, "\xc2\x80", 2},
      {0x7ff, "\xdf\xbf", 2}, {0x800, "\xe0\xa0\x80", 3},
      {0xd7ff, "\xed\x9f\xbf", 3}, {0xe000, "\xee\x80\x80", 3},
      {0xffff, "\xef\xbf\xbf", 3}, {0x10000, "\xf0\x90\x80\x80", 4},
      {0x10ffff, "\xf4\x8f\xbf\xbf", 4}, {0x1f642, "\xf0\x9f\x99\x82", 4}};
  for (const auto& sample : valid) {
    const std::string tag = " scalar " + std::to_string(sample.code);
    for (size_t split = 0; split < sample.size; ++split) {
      State state = initialized();
      wchar_t out[3] = {0x55, 0x66, 0x77};
      errno = 0;
      check(api.decode(out + 1, sample.bytes, split, &state) == partial && out[1] == 0x66,
            "decode incomplete/no output" + tag);
      check(api.decode(out + 1, sample.bytes + split, sample.size - split, &state) ==
                    (sample.code == 0 ? 0 : sample.size - split) &&
                out[0] == 0x55 && out[1] == sample.code && out[2] == 0x77 &&
                api.initial(&state) && reserved(state) && errno == 0,
            "decode split/state/guard" + tag);
      state = initialized();
      char32_t out32 = 0x55;
      check(api.decode32(&out32, sample.bytes, sample.size, &state) ==
                    (sample.code == 0 ? 0 : sample.size) && out32 == static_cast<char32_t>(sample.code),
            "C11 decode32" + tag);
    }
    State state = initialized();
    char out[6];
    std::memset(out, 0x55, sizeof(out));
    check(api.encode(out + 1, sample.code, &state) == sample.size && out[0] == 0x55 &&
              out[sample.size + 1] == 0x55 && !std::memcmp(out + 1, sample.bytes, sample.size) &&
              api.initial(&state) && reserved(state), "encode/state/guard" + tag);
  }
  const char* invalid[] = {"\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80",
      "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
      "\xf8\x80\x80\x80", "\xe2\x41", "\xe2\x82"};
  for (const char* bytes : invalid) {
    State state = initialized();
    wchar_t out = 0x55;
    errno = 0;
    size_t result = api.decode(&out, bytes, std::strlen(bytes) + 1, &state);
    check(result == bad && errno == 84 && out == 0x55 && api.initial(&state) && reserved(state),
          "invalid UTF-8 resets/no output");
  }
  State state = initialized();
  wchar_t scalar = 0x55;
  api.decode(&scalar, "\xe2", 1, &state);
  errno = 0;
  check(api.decode(&scalar, nullptr, 99, &state) == bad && errno == 84 && api.initial(&state),
        "reset request while decode incomplete rejects NUL and resets");
  auto* raw = reinterpret_cast<unsigned char*>(&state);
  raw[3] = 1;
  errno = 0;
  check(api.decode(&scalar, "A", 1, &state) == bad && errno == 22 && api.initial(&state),
        "invalid fourth state byte EINVAL");
  char output[12];
  for (char32_t c : {char32_t{0xd800}, char32_t{0xdfff}, char32_t{0x110000}, char32_t{0x1fffff}}) {
    state = initialized();
    std::memset(output, 0x55, sizeof(output));
    errno = 0;
    check(api.encode32(output + 1, c, &state) == (c < 0x10000 ? 3u : 4u) && errno == 0 &&
              output[0] == 0x55 && output[c < 0x10000 ? 4 : 5] == 0x55,
          "pinned encoder accepts surrogate/non-scalar asymmetry");
  }
  state = initialized();
  errno = 0;
  check(api.encode32(output, 0x200000, &state) == bad && errno == 84,
        "encoder upper bound EILSEQ");
  api.decode(&scalar, "\xe2", 1, &state);
  errno = 0;
  check(api.encode(output, 'A', &state) == bad && errno == 84 && api.initial(&state),
        "encode rejects pending decode state");
  api.decode(&scalar, "\xe2", 1, &state);
  check(api.encode(nullptr, 0x200000, &state) == 1 && api.initial(&state) && reserved(state),
        "null encode resets despite scalar argument");

  state = initialized();
  char16_t unit = 0x55;
  check(api.decode16(&unit, "\xf0\x9f", 2, &state) == partial && unit == 0x55,
        "UTF16 split input");
  check(api.decode16(&unit, "\x99\x82", 2, &state) == 2 && unit == 0xd83d && !api.initial(&state),
        "UTF16 leading surrogate returns bytes consumed now");
  check(api.decode16(&unit, "A", 0, &state) == pending && unit == 0xde42 && api.initial(&state),
        "UTF16 pending trail consumes no input even n zero");
  check(api.decode16(&unit, "A", 1, &state) == 1 && unit == 'A', "UTF16 resumes next input");
  unit = 0x55;
  check(api.decode16(&unit, "", 1, &state) == 0 && unit == 0x55,
        "pinned UTF16 NUL leaves output untouched");
  check(api.decode16(nullptr, "\xf0\x9f\x99\x82", 4, &state) == 4 &&
            api.decode16(nullptr, nullptr, 0, &state) == pending && api.initial(&state),
        "UTF16 discarded output still queues trail before reset request");
  state = initialized();
  std::memset(output, 0x55, sizeof(output));
  check(api.encode16(output, 0xd83d, &state) == 0 && output[0] == 0x55 && !api.initial(&state),
        "UTF16 encode stores high surrogate/no output");
  check(api.encode16(output, 0xde42, &state) == 4 &&
            !std::memcmp(output, "\xf0\x9f\x99\x82", 4) && api.initial(&state) && reserved(state),
        "UTF16 encode pair/state");
  check(api.encode16(output, 0xd800, &state) == 0 && api.initial(&state),
        "pinned D800 high surrogate has zero state");
  errno = 0;
  check(api.encode16(output, 0xdc00, &state) == bad && errno == 22 && api.initial(&state),
        "pinned D800/DC00 pair fails EINVAL");
  api.encode16(output, 0xd801, &state);
  errno = 0;
  check(api.encode16(nullptr, 'A', &state) == bad && errno == 22 && api.initial(&state),
        "pinned UTF16 null output does not bypass pending validation");

  wchar_t wide[5] = {0x55, 0x55, 0x55, 0x55, 0x55};
  const char* bytes = "A\xf0\x9f\x99\x82Z";
  const char* source = bytes;
  state = initialized();
  check(api.decode_string(wide + 1, &source, 2, &state) == 2 && source == bytes + 5 &&
            wide[0] == 0x55 && wide[1] == 'A' && wide[2] == 0x1f642 && wide[3] == 0x55,
        "wide string output bound/source/guards");
  check(api.decode_string(wide, &source, 2, &state) == 1 && source == nullptr && wide[1] == 0,
        "wide string consumes NUL");
  source = bytes;
  check(api.decode_string(nullptr, &source, 0, &state) == 3 && source == bytes && api.initial(&state),
        "wide length query ignores output bound/preserves source");
  source = bytes;
  errno = 0;
  check(api.decode_bounded(wide, &source, 3, 5, &state) == bad && errno == 84 &&
            source == bytes + 3 && api.initial(&state), "pinned bounded incomplete fails/advances/resets");
  source = bytes;
  errno = 0;
  check(api.decode_bounded(nullptr, &source, 3, 0, &state) == bad && errno == 84 && source == bytes &&
            api.initial(&state), "pinned bounded incomplete query fails/preserves source");
  source = "\xc0\x80";
  const char* original = source;
  errno = 0;
  check(api.decode_bounded(wide, &source, 2, 0, &state) == 0 && source == original && errno == 0,
        "zero output bound does not inspect input");
  const wchar_t* wsource = L"A\U0001f642Z";
  const wchar_t* woriginal = wsource;
  std::memset(output, 0x55, sizeof(output));
  check(api.encode_string(output + 1, &wsource, 4, &state) == 1 && wsource == woriginal + 1 &&
            output[0] == 0x55 && output[1] == 'A' && output[2] == 0x55,
        "UTF8 string bound never splits/source/guards");
  check(api.encode_bounded(output, &wsource, 1, 4, &state) == 4 && wsource == woriginal + 2,
        "UTF8 string source count bound");
  check(api.encode_string(output, &wsource, 1, &state) == 1 && wsource == woriginal + 3,
        "UTF8 exactly full buffer leaves NUL pending");
  check(api.encode_string(output, &wsource, 0, &state) == 0 && wsource == woriginal + 3,
        "zero UTF8 buffer leaves NUL/source untouched");
  check(api.encode_string(output, &wsource, 1, &state) == 0 && wsource == nullptr && output[0] == 0,
        "UTF8 string terminates only with room");
  wsource = woriginal;
  check(api.encode_string(nullptr, &wsource, 0, &state) == 6 && wsource == woriginal,
        "UTF8 length query preserves source");
  check(api.legacy_decode_string(wide, bytes, 1) == 1 && wide[0] == 'A' &&
            api.legacy_encode_string(output, woriginal, 4) == 1 && output[0] == 'A',
        "legacy string conversions share bounds");
  api.legacy_decode(nullptr, nullptr, 0);
  errno = 0;
  check(api.legacy_decode(&scalar, "\xe2", 1) == -1 && errno == 84,
        "legacy mbtowc incomplete reports EILSEQ");
  check(api.legacy_decode(&scalar, "\x82\xac", 2) == 2 && scalar == 0x20ac,
        "pinned legacy mbtowc retains incomplete state");
  api.legacy_decode(nullptr, nullptr, 0);
  errno = 0;
  check(api.legacy_length("\xe2", 1) == -2 && errno == 0,
        "pinned mblen exposes incomplete minus two");
  check(api.legacy_encode(output, 0) == 1 && output[0] == 0 &&
            api.legacy_encode(nullptr, 0x200000) == 0, "legacy encode NUL/reset");

  const struct { const wchar_t* text; int base; long signed_result; unsigned long unsigned_result;
                 size_t end; int signed_errno; int unsigned_errno; } integers[] = {
      {L"9223372036854775807!", 10, LONG_MAX, LONG_MAX, 19, 0, 0},
      {L"9223372036854775808!", 10, LONG_MAX, 0x8000000000000000ul, 19, 34, 0},
      {L"-9223372036854775808!", 10, LONG_MIN, 0x8000000000000000ul, 20, 0, 0},
      {L"-9223372036854775809!", 10, LONG_MIN, 0x7ffffffffffffffful, 20, 34, 0},
      {L"18446744073709551615!", 10, LONG_MAX, ULONG_MAX, 20, 34, 0},
      {L"18446744073709551616!", 10, LONG_MAX, ULONG_MAX, 20, 34, 34},
      {L"-18446744073709551616!", 10, LONG_MIN, ULONG_MAX, 21, 34, 34},
      {L" \t-0x100000000!", 0, -0x100000000l, 0xffffffff00000000ul, 14, 0, 0},
      {L"0b101010!", 0, 42, 42, 8, 0, 0}, {L"0b101010!", 2, 42, 42, 8, 0, 0},
      {L"0779", 0, 63, 63, 3, 0, 0}, {L"0x!", 0, 0, 0, 1, 0, 0},
      {L"0b2!", 0, 0, 0, 0, 0, 0}, {L" \t+!", 10, 0, 0, 0, 0, 0},
      {L"z!", 36, 35, 35, 1, 0, 0}, {L"12", 1, 0, 0, 0, 22, 22},
      {L"\u20031", 10, 0, 0, 0, 0, 0}, {L"123\U0001f642", 10, 123, 123, 3, 0, 0}};
  for (const auto& sample : integers) {
    wchar_t* end;
    errno = 0;
    long value = api.integer(sample.text, &end, sample.base);
    check(value == sample.signed_result && end == sample.text + sample.end && errno == sample.signed_errno,
          "LP64 signed value/errno/full-width end " + std::to_string(sample.end));
    errno = 0;
    unsigned long unsigned_value = api.unsigned_integer(sample.text, &end, sample.base);
    check(unsigned_value == sample.unsigned_result && end == sample.text + sample.end &&
              errno == sample.unsigned_errno, "LP64 unsigned value/errno/end");
  }
  std::wstring long_input(600, L'0');
  long_input += L"123!";
  wchar_t* end;
  errno = 0;
  check(api.integer(long_input.c_str(), &end, 10) == 123 && end == long_input.c_str() + 603 && errno == 0,
        "integer has no fixed input buffer");
  for (uint32_t high : {UINT32_C(0x80000000), UINT32_C(0xffffffff)}) {
    wchar_t text[] = {static_cast<wchar_t>(high), L'1', 0};
    errno = 0;
    check(api.integer(text, &end, 10) == 0 && end == text && errno == 0 &&
              api.unsigned_integer(text, &end, 10) == 0 && end == text && errno == 0,
          "integer high-bit wchar input stops in both signedness ABIs");
  }
}

template <class Char, class Compare, class CompareN, class Memory, class Check>
void WideComparisonContracts(Compare compare, CompareN compare_n, Memory memory, Check check) {
  const struct { uint32_t left; uint32_t right; int result; } samples[] = {
      {0x1f642, 0x10000, 0xf642}, {0xffffffff, 0, -1}, {0, 0xffffffff, 1},
      {0x80000000, 0, INT32_MIN}, {0x7fffffff, 0, INT32_MAX},
      {0x80000000, 0xffffffff, -2147483647}, {0xffffffff, 0x10000, -65537}};
  for (const auto& sample : samples) {
    const Char left[] = {static_cast<Char>(sample.left), 0};
    const Char right[] = {static_cast<Char>(sample.right), 0};
    check(compare(left, right) == sample.result && compare_n(left, right, 1) == sample.result &&
              memory(left, right, 1) == sample.result && compare_n(left, right, 0) == 0 &&
              memory(left, right, 0) == 0, "pinned full-width comparison subtraction/result");
    const Char prefix_left[] = {static_cast<Char>('A'), left[0], 0};
    const Char prefix_right[] = {static_cast<Char>('A'), right[0], 0};
    check(compare(prefix_left, prefix_right) == sample.result &&
              compare_n(prefix_left, prefix_right, 1) == 0 &&
              compare_n(prefix_left, prefix_right, 2) == sample.result,
          "pinned full-width comparison bounded prefix");
  }
}

template <class Double, class Float, class Check>
void WideFloatingContracts(Double to_double, Float to_float, Check check) {
  const struct { const wchar_t* text; double value; size_t end; } numbers[] = {
      {L"\u2003-0x1.8p2!", -6, 9}, {L"0xap0!", 10, 5},
      // bionic/wcstod.cpp's candidate alphabet excludes b/c/d hex digits.
      {L"0xabc!", 10, 3}, {L"0x.p2!", 0, 1}, {L"1e+!", 1, 1},
      {L"1.25e-2!", 0.0125, 7}, {L".5!", 0.5, 2}, {L" \t+!", 0, 0}};
  for (const auto& sample : numbers) {
    wchar_t* end;
    errno = 0;
    double value = to_double(sample.text, &end);
    check(value == sample.value && end == sample.text + sample.end && errno == 0,
          "pinned floating grammar/value/end");
  }
  for (const wchar_t* text : {L"nan(a)!", L"nan(b)!", L"NAN!"}) {
    wchar_t* end;
    size_t expected = text[3] == '(' && text[4] == 'a' ? 6 : 3;
    check(std::isnan(to_double(text, &end)) && end == text + expected,
          "pinned NaN payload candidate/rollback");
  }
  const wchar_t* infinity = L"+infinity!";
  wchar_t* end;
  check(std::isinf(to_double(infinity, &end)) && end == infinity + 9, "pinned infinity end");
  const wchar_t* overflow = L"1e9999!";
  errno = 0;
  check(std::isinf(to_double(overflow, &end)) && errno == 34 && end == overflow + 6,
        "floating overflow errno/end");
  const wchar_t* underflow = L"-1e-9999!";
  errno = 0;
  double zero = to_double(underflow, &end);
  check(zero == 0 && std::signbit(zero) && errno == 34 && end == underflow + 8,
        "floating underflow sign/errno/end");
  const wchar_t* rounded = L"16777217!";
  check(to_float(rounded, &end) == 16777216.0f && to_double(rounded, &end) == 16777217.0 &&
            end == rounded + 8, "binary32 rounds directly");
  const wchar_t* above_halfway =
      L"1.0000000596046447753906250000000000000000000000000000000000000000000000000000000000001!";
  check(to_float(above_halfway, &end) == 0x1.000002p0f && *end == L'!' &&
            to_double(above_halfway, &end) == 0x1.000001p0 && *end == L'!',
        "binary32 conversion avoids double rounding at halfway");
  std::wstring long_float(600, L'0');
  long_float += L"1.25e2!";
  check(to_double(long_float.c_str(), &end) == 125 && end == long_float.c_str() + 606,
        "floating input/end beyond 255 characters");
  for (uint32_t high : {UINT32_C(0x80000000), UINT32_C(0xffffffff)}) {
    wchar_t text[] = {static_cast<wchar_t>(high), L'1', 0};
    errno = 0;
    check(to_double(text, &end) == 0 && end == text && to_float(text, &end) == 0 && end == text && errno == 0,
          "floating high-bit wchar input stops in both signedness ABIs");
  }
}

template <class Format, class Check>
void WideTimeContracts(Format format, Check check) {
  static_assert(sizeof(struct tm) == 56 && offsetof(struct tm, tm_gmtoff) == 40 &&
                offsetof(struct tm, tm_zone) == 48);
  struct tm time{};
  time.tm_year = 124; time.tm_mon = 1; time.tm_mday = 29;
  time.tm_hour = 23; time.tm_min = 7; time.tm_sec = 6;
  time.tm_wday = 4; time.tm_yday = 59; time.tm_gmtoff = 19800; time.tm_zone = "TEST";
  wchar_t out[128];
  std::fill(std::begin(out), std::end(out), 0x55);
  const wchar_t* expected = L"\U0001f642 2024-02-29 23:07:06 +0530 TEST";
  check(format(out, 128, L"\U0001f642 %Y-%m-%d %H:%M:%S %z %Z", &time) == wcslen(expected) &&
            !wcscmp(out, expected), "pinned wide time UTF32/tm/zone output");
  std::fill(std::begin(out), std::end(out), 0x55);
  check(format(out + 1, 4, L"\U0001f642", &time) == 0 && out[0] == 0x55 && out[1] == 0x55 && out[5] == 0x55,
        "pinned wide time narrow byte bound/guards");
  check(format(out + 1, 5, L"\U0001f642", &time) == 1 && out[1] == 0x1f642 && out[2] == 0 && out[3] == 0x55,
        "pinned wide time supplementary output uses one unit");
  out[1] = 0x55;
  check(format(out + 1, 0, L"%Y", &time) == 0 && out[1] == 0x55, "wide time zero output bound");
  errno = 0;
  check(format(out + 1, SIZE_MAX, L"%Y", &time) == 0 && errno == 22 && out[1] == 0x55,
        "wide time overflow guard/error");
  const wchar_t invalid[] = {static_cast<wchar_t>(0x200000), 0};
  for (size_t max : {size_t{0}, size_t{128}, SIZE_MAX}) {
    errno = 0;
    check(format(out + 1, max, invalid, &time) == 0 && errno == 84 && out[1] == 0x55,
          "pinned wide time invalid format precedes zero/overflow bounds");
  }
  errno = 0;
  check(format(out + 1, 128, L"", &time) == 0 && errno == 0 && out[1] == 0x55,
        "pinned wide time empty format preserves destination");
  time.tm_zone = "\xf0\x9f\x99\x82";
  check(format(out + 1, 5, L"%Z", &time) == 1 && out[1] == 0x1f642 && out[2] == 0 && out[3] == 0x55,
        "pinned wide time UTF8 zone becomes UTF32");
  time.tm_zone = "\xed\xa0\x80";
  out[1] = 0x55;
  errno = 0;
  check(format(out + 1, 128, L"%Z", &time) == 0 && errno == 84 && out[1] == 0x55,
        "pinned wide time malformed zone errors without wide output");
}
