Part Wide() {
  Part p{"wide"};
  WideApi<mbstate_t> api{};
  bool complete = true;
#define WIDE_ENTRY(field, symbol) do { \
    api.field = reinterpret_cast<decltype(api.field)>(dlsym(RTLD_DEFAULT, symbol)); \
    p.Check(api.field != nullptr, symbol); \
    complete &= api.field != nullptr; \
  } while (false)
  WIDE_ENTRY(decode, "mbrtowc");
  WIDE_ENTRY(decode32, "mbrtoc32");
  WIDE_ENTRY(decode16, "mbrtoc16");
  WIDE_ENTRY(encode, "wcrtomb");
  WIDE_ENTRY(encode32, "c32rtomb");
  WIDE_ENTRY(encode16, "c16rtomb");
  WIDE_ENTRY(length, "mbrlen");
  WIDE_ENTRY(initial, "mbsinit");
  WIDE_ENTRY(legacy_decode, "mbtowc");
  WIDE_ENTRY(legacy_length, "mblen");
  WIDE_ENTRY(legacy_encode, "wctomb");
  WIDE_ENTRY(decode_string, "mbsrtowcs");
  WIDE_ENTRY(decode_bounded, "mbsnrtowcs");
  WIDE_ENTRY(encode_string, "wcsrtombs");
  WIDE_ENTRY(encode_bounded, "wcsnrtombs");
  WIDE_ENTRY(legacy_decode_string, "mbstowcs");
  WIDE_ENTRY(legacy_encode_string, "wcstombs");
  WIDE_ENTRY(integer, "wcstol");
  WIDE_ENTRY(unsigned_integer, "wcstoul");
#undef WIDE_ENTRY
  if (!complete) return p;
  WideContracts(api, [&](bool ok, const std::string& what) { p.Check(ok, what); });
  p.notes.push_back("pinned UTF8/16/32 state, errors, bounds and LP64 integers");

  locale_t locale = newlocale(LC_ALL_MASK, "C.UTF-8", nullptr);
  p.Check(locale != nullptr, "newlocale all categories");
  if (locale == nullptr) return p;
  locale_t copy = duplocale(locale);
  p.Check(copy != nullptr && copy != locale, "duplocale independent allocation");
  locale_t previous = uselocale(locale);
  p.Check(MB_CUR_MAX == 4, "UTF8 locale layout");
  locale_t plain = newlocale(LC_CTYPE_MASK, "C", nullptr);
  p.Check(plain != nullptr && plain != locale, "C locale independent allocation");
  if (plain != nullptr) {
    uselocale(plain);
    p.Check(MB_CUR_MAX == 1, "C locale layout");
    wchar_t character;
    mbstate_t state{};
    p.Check(api.decode(&character, "\xc2\x80", 2, &state) == 2 && character == 0x80,
            "pinned C locale still decodes UTF8");
    uselocale(locale);
  }
  errno = 0;
  p.Check(newlocale(1 << LC_ALL, "C", nullptr) == nullptr && errno == EINVAL,
          "invalid category mask EINVAL");
  errno = 0;
  using NewLocale = locale_t (*)(int, const char*, locale_t);
  auto new_locale = reinterpret_cast<NewLocale>(dlsym(RTLD_DEFAULT, "newlocale"));
  p.Check(new_locale && new_locale(LC_CTYPE_MASK, nullptr, nullptr) == nullptr && errno == EINVAL,
          "null locale name EINVAL");
  errno = 0;
  p.Check(newlocale(LC_ALL_MASK, "fr_FR.UTF-8", nullptr) == nullptr && errno == ENOENT,
          "unsupported locale ENOENT");
  errno = 0;
  p.Check(setlocale(13, "C") == nullptr && errno == EINVAL, "setlocale invalid category");
  errno = 0;
  p.Check(setlocale(LC_ALL, "unsupported") == nullptr && errno == ENOENT,
          "setlocale unsupported name");
  p.Check(!std::strcmp(localeconv()->decimal_point, ".") && localeconv()->grouping[0] == 0,
          "C locale lconv pointer layout");
  using DecodeL = size_t (*)(wchar_t*, const char**, size_t, mbstate_t*, locale_t);
  using EncodeL = size_t (*)(char*, const wchar_t**, size_t, mbstate_t*, locale_t);
  auto decode_l = reinterpret_cast<DecodeL>(dlsym(RTLD_DEFAULT, "mbsrtowcs_l"));
  auto encode_l = reinterpret_cast<EncodeL>(dlsym(RTLD_DEFAULT, "wcsrtombs_l"));
  p.Check(decode_l && encode_l, "restartable conversion locale aliases");
  if (decode_l && encode_l) {
    mbstate_t state{};
    const char* bytes = "\xf0\x9f\x99\x82";
    const wchar_t* wide = L"\U0001f642";
    wchar_t decoded[2];
    char encoded[5];
    p.Check(decode_l(decoded, &bytes, 2, &state, locale) == 1 && bytes == nullptr &&
                decoded[0] == 0x1f642 && decoded[1] == 0 &&
                encode_l(encoded, &wide, sizeof(encoded), &state, locale) == 4 && wide == nullptr &&
                !std::memcmp(encoded, "\xf0\x9f\x99\x82", 5), "locale conversions use UTF32/state/source ABI");
  }

  const wchar_t* large = L"-9223372036854775808!";
  wchar_t* end = nullptr;
  using Signed = int64_t (*)(const wchar_t*, wchar_t**, int);
  using Unsigned = uint64_t (*)(const wchar_t*, wchar_t**, int);
  using SignedL = int64_t (*)(const wchar_t*, wchar_t**, int, locale_t);
  using UnsignedL = uint64_t (*)(const wchar_t*, wchar_t**, int, locale_t);
  for (const char* name : {"wcstoll", "wcstoimax"}) {
    auto fn = reinterpret_cast<Signed>(dlsym(RTLD_DEFAULT, name));
    p.Check(fn != nullptr && fn(large, &end, 10) == INT64_MIN && end == large + 20,
            std::string(name) + " LP64 alias");
  }
  for (const char* name : {"wcstol_l", "wcstoll_l"}) {
    auto fn = reinterpret_cast<SignedL>(dlsym(RTLD_DEFAULT, name));
    p.Check(fn != nullptr && fn(large, &end, 10, locale) == INT64_MIN && end == large + 20,
            std::string(name) + " LP64 locale alias");
  }
  large = L"18446744073709551615!";
  for (const char* name : {"wcstoull", "wcstoumax"}) {
    auto fn = reinterpret_cast<Unsigned>(dlsym(RTLD_DEFAULT, name));
    p.Check(fn != nullptr && fn(large, &end, 10) == UINT64_MAX && end == large + 20,
            std::string(name) + " unsigned LP64 alias");
  }
  for (const char* name : {"wcstoul_l", "wcstoull_l"}) {
    auto fn = reinterpret_cast<UnsignedL>(dlsym(RTLD_DEFAULT, name));
    p.Check(fn != nullptr && fn(large, &end, 10, locale) == UINT64_MAX && end == large + 20,
            std::string(name) + " unsigned LP64 locale alias");
  }

  std::wstring long_float(600, L'0');
  long_float += L"1.25e2!";
  using Double = double (*)(const wchar_t*, wchar_t**);
  using Float = float (*)(const wchar_t*, wchar_t**);
  using DoubleL = double (*)(const wchar_t*, wchar_t**, locale_t);
  using FloatL = float (*)(const wchar_t*, wchar_t**, locale_t);
  auto to_double = reinterpret_cast<Double>(dlsym(RTLD_DEFAULT, "wcstod"));
  auto to_float = reinterpret_cast<Float>(dlsym(RTLD_DEFAULT, "wcstof"));
  auto double_l = reinterpret_cast<DoubleL>(dlsym(RTLD_DEFAULT, "wcstod_l"));
  auto float_l = reinterpret_cast<FloatL>(dlsym(RTLD_DEFAULT, "wcstof_l"));
  p.Check(to_double && to_float && double_l && float_l, "float and double entry points");
  if (to_double && to_float && double_l && float_l) {
    WideFloatingContracts(to_double, to_float,
        [&](bool ok, const std::string& what) { p.Check(ok, what); });
    p.Check(to_double(long_float.c_str(), &end) == 125 && end == long_float.c_str() + 606 &&
                float_l(long_float.c_str(), &end, locale) == 125 && end == long_float.c_str() + 606,
            "float input/end beyond 255 characters");
    const wchar_t* fraction = L"\u2003-0x1.8p2\U0001f642";
    p.Check(to_float(fraction, &end) == -6 && end == fraction + 9 &&
                double_l(fraction, &end, locale) == -6 && end == fraction + 9,
            "float Unicode whitespace/hex/full-width terminator");
    const wchar_t* invalid = L" \t+!";
    p.Check(to_double(invalid, &end) == 0 && end == invalid, "float no conversion resets end");
    const wchar_t* overflow = L"1e9999!";
    errno = 0;
    p.Check(std::isinf(to_double(overflow, &end)) && errno == ERANGE && end == overflow + 6,
            "double overflow errno/end");
    const wchar_t* underflow = L"-1e-9999!";
    errno = 0;
    double zero = double_l(underflow, &end, locale);
    p.Check(zero == 0 && std::signbit(zero) && errno == ERANGE && end == underflow + 8,
            "double underflow sign/errno/end");
    const wchar_t* rounded = L"16777217!";
    p.Check(to_float(rounded, &end) == 16777216.0f && to_double(rounded, &end) == 16777217.0,
            "float rounds directly to binary32");
  }

  const wchar_t* transform = L"A\U0001f642Z";
  using Transform = size_t (*)(wchar_t*, const wchar_t*, size_t);
  using TransformL = size_t (*)(wchar_t*, const wchar_t*, size_t, locale_t);
  using Collate = int (*)(const wchar_t*, const wchar_t*);
  using CollateL = int (*)(const wchar_t*, const wchar_t*, locale_t);
  auto xfrm = reinterpret_cast<Transform>(dlsym(RTLD_DEFAULT, "wcsxfrm"));
  auto xfrm_l = reinterpret_cast<TransformL>(dlsym(RTLD_DEFAULT, "wcsxfrm_l"));
  auto collate = reinterpret_cast<Collate>(dlsym(RTLD_DEFAULT, "wcscoll"));
  auto collate_l = reinterpret_cast<CollateL>(dlsym(RTLD_DEFAULT, "wcscoll_l"));
  p.Check(xfrm && xfrm_l && collate && collate_l, "wide collation entry points");
  if (xfrm && xfrm_l && collate && collate_l) {
    wchar_t guard[6] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    p.Check(xfrm(nullptr, transform, 0) == 3 && xfrm(guard + 1, transform, 2) == 3 &&
                guard[0] == 0x55 && guard[1] == 'A' && guard[2] == 0 && guard[3] == 0x55,
            "pinned bounded wcsxfrm is wcslcpy");
    p.Check(xfrm_l(guard + 1, transform, 4, locale) == 3 && guard[2] == 0x1f642 &&
                guard[3] == 'Z' && guard[4] == 0 && guard[5] == 0x55,
            "wide transform UTF32 units/locale alias");
    p.Check(collate(L"\U00010000", L"\uffff") > 0 &&
                collate_l(L"Z", L"a", locale) < 0 && collate(transform, transform) == 0,
            "pinned C collation orders codepoints");
  }
  using Time = size_t (*)(wchar_t*, size_t, const wchar_t*, const struct tm*);
  using TimeL = size_t (*)(wchar_t*, size_t, const wchar_t*, const struct tm*, locale_t);
  auto format_time = reinterpret_cast<Time>(dlsym(RTLD_DEFAULT, "wcsftime"));
  auto time_l = reinterpret_cast<TimeL>(dlsym(RTLD_DEFAULT, "wcsftime_l"));
  p.Check(format_time && time_l && sizeof(struct tm) == 56 && offsetof(struct tm, tm_gmtoff) == 40 &&
              offsetof(struct tm, tm_zone) == 48, "wide time entry/layout");
  if (format_time && time_l) {
    struct tm time{};
    time.tm_year = 124; time.tm_mon = 1; time.tm_mday = 29;
    time.tm_hour = 23; time.tm_min = 7; time.tm_sec = 6;
    time.tm_wday = 4; time.tm_yday = 59; time.tm_gmtoff = 19800; time.tm_zone = "TEST";
    wchar_t out[100];
    const wchar_t* pattern = L"\U0001f642 %Y-%m-%d %H:%M:%S %z %Z";
    const wchar_t* expected = L"\U0001f642 2024-02-29 23:07:06 +0530 TEST";
    p.Check(format_time(out, 100, pattern, &time) == wcslen(expected) && !wcscmp(out, expected) &&
                time_l(out, 100, pattern, &time, locale) == wcslen(expected) && !wcscmp(out, expected),
            "wide time UTF32 output/gmtoff/zone/locale");
    std::fill(std::begin(out), std::end(out), 0x55);
    p.Check(format_time(out + 1, 4, L"\U0001f642", &time) == 0 && out[0] == 0x55 && out[1] == 0x55 &&
                out[5] == 0x55, "pinned wide time applies narrow byte bound");
    p.Check(format_time(out + 1, 5, L"\U0001f642", &time) == 1 && out[1] == 0x1f642 &&
                out[2] == 0 && out[3] == 0x55, "wide time supplementary output uses one unit");
    p.Check(format_time(out, 0, L"%Y", &time) == 0, "wide time zero buffer");
    errno = 0;
    p.Check(format_time(out, SIZE_MAX, L"%Y", &time) == 0 && errno == EINVAL,
            "wide time rejects size multiplication overflow");
  }
  uselocale(previous);
  if (plain) freelocale(plain);
  if (copy) freelocale(copy);
  freelocale(locale);
  p.notes.push_back("wide locale aliases, floating parsing, C collation and UTF32 time");
  return p;
}
