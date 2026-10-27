// Plain and locale ABI entry points against the independent pinned ICU4X
// oracle.
#include "native3_unicode_samples.inc"
#include "native3_unicode_width_samples.inc"

Part Unicode() {
  Part p{"unicode"};
  locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
  p.Check(locale != nullptr, "UTF-8 locale");
  if (locale == nullptr) return p;
  const char* names[] = {"alnum", "alpha",  "blank",   "cntrl",  "digit",
                         "graph", "lower",  "print",   "punct",  "space",
                         "upper", "xdigit", "tolower", "toupper"};
  using Predicate = int (*)(wint_t);
  using PredicateL = int (*)(wint_t, locale_t);
  using Mapping = wint_t (*)(wint_t);
  using MappingL = wint_t (*)(wint_t, locale_t);
  for (size_t i = 0; i < 14; ++i) {
    std::string name = i < 12    ? std::string("isw") + names[i]
                       : i == 12 ? "towlower"
                                 : "towupper";
    void* plain = dlsym(RTLD_DEFAULT, name.c_str());
    void* localized = dlsym(RTLD_DEFAULT, (name + "_l").c_str());
    p.Check(plain != nullptr && localized != nullptr,
            name + " and locale entry");
    if (plain == nullptr || localized == nullptr) continue;
    for (const UnicodeSample& sample : kUnicodeSamples) {
      wint_t c = sample.codepoint;
      uint32_t a = i < 12 ? reinterpret_cast<Predicate>(plain)(c) != 0
                          : reinterpret_cast<Mapping>(plain)(c);
      uint32_t b = i < 12
                       ? reinterpret_cast<PredicateL>(localized)(c, locale) != 0
                       : reinterpret_cast<MappingL>(localized)(c, locale);
      p.Check(a == sample.values[i] && b == sample.values[i],
              name + " oracle code point " + std::to_string(sample.codepoint));
      if (i < 12) {
        wctype_t type = wctype(names[i]);
        p.Check(type != 0 && wctype_l(names[i], locale) == type &&
                    static_cast<uint32_t>(iswctype(c, type) != 0) ==
                        sample.values[i] &&
                    static_cast<uint32_t>(iswctype_l(c, type, locale) != 0) ==
                        sample.values[i],
                "wctype " + std::string(names[i]));
      } else {
        wctrans_t type = wctrans(names[i]);
        p.Check(type != nullptr && wctrans_l(names[i], locale) == type &&
                    towctrans(c, type) == sample.values[i] &&
                    towctrans_l(c, type, locale) == sample.values[i],
                "wctrans " + std::string(names[i]));
      }
    }
  }
  p.Check(wctype("not-a-class") == 0 && iswctype(0x345, 0) == 0,
          "invalid wctype handle");
  p.Check(wctrans("not-a-mapping") == nullptr && errno == EINVAL,
          "invalid wctrans sets EINVAL");
  p.Check(wctrans_l("not-a-mapping", locale) == nullptr && errno == EINVAL,
          "invalid locale wctrans sets EINVAL");
  auto bad = reinterpret_cast<wctrans_t>(static_cast<uintptr_t>(0x1234));
  p.Check(towctrans(0x345, bad) == 0x345 && errno == EINVAL,
          "invalid towctrans preserves input and sets EINVAL");
  p.Check(towctrans_l(0x345, bad, locale) == 0x345 && errno == EINVAL,
          "invalid locale towctrans preserves input and sets EINVAL");
  using Width = int (*)(wchar_t);
  auto width = reinterpret_cast<Width>(dlsym(RTLD_DEFAULT, "wcwidth"));
  p.Check(width != nullptr, "wcwidth entry");
  if (width != nullptr) {
    for (const UnicodeWidthSample& sample : kUnicodeWidthSamples) {
      p.Check(width(static_cast<wchar_t>(sample.codepoint)) == WidthExpected(sample),
              "wcwidth oracle code point " + std::to_string(sample.codepoint));
    }
    uint64_t digest = UINT64_C(0xcbf29ce484222325);
    auto add_width = [&](uint32_t c) {
      uint32_t value = static_cast<uint32_t>(width(static_cast<wchar_t>(c)));
      for (unsigned shift = 0; shift < 32; shift += 8) {
        digest = (digest ^ ((value >> shift) & 0xff)) * UINT64_C(0x100000001b3);
      }
    };
    for (uint32_t c = 0; c <= 0x10ffff; ++c) add_width(c);
    for (uint32_t c : {UINT32_C(0x110000), UINT32_C(0x7fffffff),
                       UINT32_C(0x80000000), UINT32_C(0xffffffff)}) add_width(c);
    p.Check(digest == kUnicodeWidthDigest,
            std::string("wcwidth full-range ") + kUnicodeWidthAbi + " digest " + std::to_string(digest));
  }
  const wchar_t sequence[] = {L'A', 0x345, 0x115f, 0x1160, 0xad,
                              0x1d167, 0x1f600, 0, L'B', 0};
  p.Check(wcswidth(sequence, 3) == 3 && wcswidth(sequence, 6) == 4,
          "wcswidth bounds and Hangul combining sequence");
  p.Check(wcswidth(sequence, 10) == 6 && wcswidth(sequence, 0) == 0,
          "wcswidth NUL termination and zero count");
  const wchar_t control[] = {L'A', 0xa0, L'B', 0};
  const wchar_t invalid[] = {static_cast<wchar_t>(0xffffffff), 0};
  p.Check(wcswidth(control, 1) == 1 && wcswidth(control, 2) == -1 &&
              wcswidth(invalid, 1) == WidthExpected(kUnicodeWidthSamples[32]),
          "wcswidth controls and target-specific high-bit input");
  freelocale(locale);
  p.notes.push_back(
      "pinned ICU4X boundaries; plain/_l/wctype/wctrans and errors");
  p.notes.push_back("pinned wcwidth/wcswidth: 33 boundaries, full-width strings and bounds");
  p.notes.push_back(std::string(kUnicodeWidthAbi) + "; full-range width digest checked");
  return p;
}
