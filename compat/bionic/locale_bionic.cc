// gesher bionic shim: locales, multibyte and wide characters, for libc++.
//
// libc++_shared.so builds its std::locale facets on bionic's <locale.h>,
// <wchar.h> and <wctype.h>: newlocale/uselocale, MB_CUR_MAX, the UTF-8
// conversions (mbrtowc, wcrtomb, mbsnrtowcs, ...), the *_l classification and
// conversion functions. bionic knows the "C"/"POSIX" locale and UTF-8 ones
// ("", "C.UTF-8", "en_US.UTF-8"), all with C collation and classification;
// its default is UTF-8 (MB_CUR_MAX 4). Here as there: a locale_t is an opaque
// pointer, uselocale is per thread, and every multibyte conversion is UTF-8.
//
// wchar_t is 32-bit UTF-32 in the .so and 16-bit here, so the wide functions
// are written out on uint32_t; mbstate_t is bionic's 8 bytes, with up to three
// pending bytes of a UTF-8 sequence kept in it. long double is IEEE binary128
// there (strtold returns it in %xmm0).

#include "shim_internal.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

#include <atomic>

namespace gesher::bionic {

namespace {

using wchar32 = uint32_t;
using wint32 = uint32_t;
constexpr wint32 kWEOF = 0xffffffffu;
constexpr int kEILSEQ = 84;  // Linux
constexpr int kENOENT = 2;
constexpr size_t kInvalid = static_cast<size_t>(-1);
constexpr size_t kIncomplete = static_cast<size_t>(-2);

// --- locales -------------------------------------------------------------------

struct BionicLocale {
  size_t mb_cur_max;
};
BionicLocale g_utf8_locale{4};
BionicLocale g_c_locale{1};
// LC_GLOBAL_LOCALE is ((locale_t) -1L).
BionicLocale* const kGlobalLocale = reinterpret_cast<BionicLocale*>(static_cast<intptr_t>(-1));
std::atomic<BionicLocale*> g_global{&g_utf8_locale};
thread_local BionicLocale* t_locale = nullptr;  // nullptr: the global locale

bool IsUtf8Name(const char* name) { return *name == '\0' || std::strstr(name, "UTF-8") != nullptr; }

bool IsSupported(const char* name) {
  return name != nullptr && (*name == '\0' || std::strcmp(name, "C") == 0 ||
                             std::strcmp(name, "POSIX") == 0 ||
                             std::strcmp(name, "C.UTF-8") == 0 ||
                             std::strcmp(name, "en_US.UTF-8") == 0);
}

BionicLocale* BIONIC_SYSV Newlocale(int /*mask*/, const char* name, BionicLocale* /*base*/) {
  if (!IsSupported(name)) {
    errno = kENOENT;
    return nullptr;
  }
  return IsUtf8Name(name) ? &g_utf8_locale : &g_c_locale;
}

void BIONIC_SYSV Freelocale(BionicLocale*) {}

BionicLocale* BIONIC_SYSV Duplocale(BionicLocale* l) {
  return l == kGlobalLocale ? g_global.load() : l;
}

BionicLocale* BIONIC_SYSV Uselocale(BionicLocale* l) {
  BionicLocale* old = t_locale != nullptr ? t_locale : kGlobalLocale;
  if (l != nullptr) t_locale = l == kGlobalLocale ? nullptr : l;
  return old;
}

size_t BIONIC_SYSV CtypeGetMbCurMax() {
  return (t_locale != nullptr ? t_locale : g_global.load())->mb_cur_max;
}

char* BIONIC_SYSV Setlocale(int /*category*/, const char* name) {
  static char utf8[] = "C.UTF-8";
  static char c[] = "C";
  if (name != nullptr) {
    if (!IsSupported(name)) return nullptr;
    g_global.store(IsUtf8Name(name) ? &g_utf8_locale : &g_c_locale);
  }
  return g_global.load() == &g_utf8_locale ? utf8 : c;
}

// bionic's struct lconv (POSIX order, no wide members).
struct BionicLconv {
  const char* decimal_point;
  const char* thousands_sep;
  const char* grouping;
  const char* int_curr_symbol;
  const char* currency_symbol;
  const char* mon_decimal_point;
  const char* mon_thousands_sep;
  const char* mon_grouping;
  const char* positive_sign;
  const char* negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes,
      n_sep_by_space, p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space,
      int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};
const BionicLconv kLconv = {".", "", "", "", "", "", "", "", "", "",
                            CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
                            CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX};

const BionicLconv* BIONIC_SYSV Localeconv() { return &kLconv; }

// --- UTF-8 ---------------------------------------------------------------------

struct BionicMbstate {
  uint8_t seq[4];       // pending bytes of a sequence
  uint8_t reserved[4];  // reserved[0]: how many
};

int SequenceLength(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return -1;
}

size_t Fail(BionicMbstate* ps) {
  std::memset(ps, 0, sizeof(*ps));
  errno = kEILSEQ;
  return kInvalid;
}

size_t Mbrtowc(wchar32* pwc, const char* s, size_t n, BionicMbstate* ps) {
  static thread_local BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  if (s == nullptr) {
    s = "";
    n = 1;
    pwc = nullptr;
  }
  if (n == 0) return kIncomplete;
  uint8_t buf[4];
  size_t have = ps->reserved[0] < 4 ? ps->reserved[0] : 0;
  std::memcpy(buf, ps->seq, have);
  size_t used = 0;
  if (have == 0) buf[have++] = static_cast<uint8_t>(s[used++]);
  int len = SequenceLength(buf[0]);
  if (len < 0) return Fail(ps);
  while (have < static_cast<size_t>(len) && used < n) {
    uint8_t b = static_cast<uint8_t>(s[used++]);
    if ((b & 0xC0) != 0x80) return Fail(ps);
    buf[have++] = b;
  }
  if (have < static_cast<size_t>(len)) {
    std::memcpy(ps->seq, buf, have);
    ps->reserved[0] = static_cast<uint8_t>(have);
    return kIncomplete;
  }
  wchar32 c;
  switch (len) {
    case 1: c = buf[0]; break;
    case 2: c = ((buf[0] & 0x1Fu) << 6) | (buf[1] & 0x3Fu); break;
    case 3: c = ((buf[0] & 0x0Fu) << 12) | ((buf[1] & 0x3Fu) << 6) | (buf[2] & 0x3Fu); break;
    default:
      c = ((buf[0] & 0x07u) << 18) | ((buf[1] & 0x3Fu) << 12) | ((buf[2] & 0x3Fu) << 6) |
          (buf[3] & 0x3Fu);
      break;
  }
  static const wchar32 kMin[] = {0, 0, 0x80, 0x800, 0x10000};
  if (c < kMin[len] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return Fail(ps);
  std::memset(ps, 0, sizeof(*ps));
  if (pwc != nullptr) *pwc = c;
  return c == 0 ? 0 : used;
}

// Encodes `c` into buf; the length, or kInvalid.
size_t Encode(wchar32 c, char* buf) {
  if (c < 0x80) {
    buf[0] = static_cast<char>(c);
    return 1;
  }
  if (c < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (c >> 6));
    buf[1] = static_cast<char>(0x80 | (c & 0x3F));
    return 2;
  }
  if (c >= 0xD800 && c <= 0xDFFF) return kInvalid;
  if (c < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (c >> 12));
    buf[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (c & 0x3F));
    return 3;
  }
  if (c <= 0x10FFFF) {
    buf[0] = static_cast<char>(0xF0 | (c >> 18));
    buf[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (c & 0x3F));
    return 4;
  }
  return kInvalid;
}

size_t BIONIC_SYSV MbrtowcSysv(wchar32* pwc, const char* s, size_t n, BionicMbstate* ps) {
  return Mbrtowc(pwc, s, n, ps);
}

size_t BIONIC_SYSV Mbrlen(const char* s, size_t n, BionicMbstate* ps) {
  static thread_local BionicMbstate internal;
  return Mbrtowc(nullptr, s, n, ps != nullptr ? ps : &internal);
}

int BIONIC_SYSV Mbtowc(wchar32* pwc, const char* s, size_t n) {
  if (s == nullptr) return 0;  // stateless
  BionicMbstate st{};
  size_t r = Mbrtowc(pwc, s, n, &st);
  if (r == kInvalid || r == kIncomplete) {
    errno = kEILSEQ;
    return -1;
  }
  return static_cast<int>(r);
}

size_t BIONIC_SYSV Wcrtomb(char* s, wchar32 wc, BionicMbstate* ps) {
  if (ps != nullptr) std::memset(ps, 0, sizeof(*ps));
  if (s == nullptr) return 1;
  size_t r = Encode(wc, s);
  if (r == kInvalid) errno = kEILSEQ;
  return r;
}

wint32 BIONIC_SYSV Btowc(int c) {
  return c != EOF && static_cast<unsigned>(c) < 0x80 ? static_cast<wint32>(c) : kWEOF;
}

int BIONIC_SYSV Wctob(wint32 c) { return c < 0x80 ? static_cast<int>(c) : EOF; }

size_t BIONIC_SYSV Mbsnrtowcs(wchar32* dst, const char** src, size_t nms, size_t len,
                              BionicMbstate* ps) {
  static thread_local BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  const char* s = *src;
  size_t count = 0;
  while (dst == nullptr || count < len) {
    if (nms == 0) break;
    wchar32 c;
    size_t r = Mbrtowc(&c, s, nms, ps);
    if (r == kInvalid) {
      if (dst != nullptr) *src = s;
      return kInvalid;
    }
    if (r == kIncomplete) {
      s += nms;
      break;
    }
    if (r == 0) {
      if (dst != nullptr) {
        dst[count] = 0;
        *src = nullptr;
      }
      return count;
    }
    if (dst != nullptr) dst[count] = c;
    s += r;
    nms -= r;
    ++count;
  }
  if (dst != nullptr) *src = s;
  return count;
}

size_t BIONIC_SYSV Mbsrtowcs(wchar32* dst, const char** src, size_t len, BionicMbstate* ps) {
  return Mbsnrtowcs(dst, src, SIZE_MAX, len, ps);
}

size_t BIONIC_SYSV Wcsnrtombs(char* dst, const wchar32** src, size_t nwc, size_t len,
                              BionicMbstate* ps) {
  if (ps != nullptr) std::memset(ps, 0, sizeof(*ps));
  const wchar32* w = *src;
  size_t count = 0;
  char buf[4];
  for (; nwc > 0; ++w, --nwc) {
    if (*w == 0) {
      if (dst != nullptr) {
        if (count < len) dst[count] = '\0';
        *src = nullptr;
      }
      return count;
    }
    size_t r = Encode(*w, buf);
    if (r == kInvalid) {
      if (dst != nullptr) *src = w;
      errno = kEILSEQ;
      return kInvalid;
    }
    if (dst != nullptr) {
      if (count + r > len) break;
      std::memcpy(dst + count, buf, r);
    }
    count += r;
  }
  if (dst != nullptr) *src = w;
  return count;
}

// --- wide characters ----------------------------------------------------------------

// The CRT's classification takes 16-bit characters: the BMP goes there.
bool Bmp(wint32 c) { return c <= 0xFFFF; }

int BIONIC_SYSV Iswalpha(wint32 c, void*) {
  return c < 0x80 ? isalpha(static_cast<int>(c)) != 0 : Bmp(c) && iswalpha(static_cast<wint_t>(c));
}
int BIONIC_SYSV Iswblank(wint32 c, void*) { return c == ' ' || c == '\t'; }
int BIONIC_SYSV Iswcntrl(wint32 c, void*) { return c < 0x20 || (c >= 0x7F && c < 0xA0); }
int BIONIC_SYSV Iswdigit(wint32 c, void*) { return c >= '0' && c <= '9'; }
int BIONIC_SYSV Iswlower(wint32 c, void*) {
  return c < 0x80 ? islower(static_cast<int>(c)) != 0 : Bmp(c) && iswlower(static_cast<wint_t>(c));
}
int BIONIC_SYSV Iswupper(wint32 c, void*) {
  return c < 0x80 ? isupper(static_cast<int>(c)) != 0 : Bmp(c) && iswupper(static_cast<wint_t>(c));
}
int BIONIC_SYSV Iswprint(wint32 c, void*) {
  return c >= 0x20 && c != 0x7F && !(c >= 0x80 && c < 0xA0) && c <= 0x10FFFF;
}
int BIONIC_SYSV Iswpunct(wint32 c, void*) {
  return c < 0x80 ? ispunct(static_cast<int>(c)) != 0 : Bmp(c) && iswpunct(static_cast<wint_t>(c));
}
int BIONIC_SYSV Iswspace(wint32 c, void*) {
  return c < 0x80 ? isspace(static_cast<int>(c)) != 0 : Bmp(c) && iswspace(static_cast<wint_t>(c));
}
int BIONIC_SYSV Iswxdigit(wint32 c, void*) { return c < 0x80 && isxdigit(static_cast<int>(c)); }
wint32 BIONIC_SYSV Towlower(wint32 c, void*) {
  return Bmp(c) ? static_cast<wint32>(towlower(static_cast<wint_t>(c))) : c;
}
wint32 BIONIC_SYSV Towupper(wint32 c, void*) {
  return Bmp(c) ? static_cast<wint32>(towupper(static_cast<wint_t>(c))) : c;
}

size_t BIONIC_SYSV Wcslen(const wchar32* s) {
  size_t n = 0;
  while (s[n] != 0) ++n;
  return n;
}

wchar32* BIONIC_SYSV Wmemchr(const wchar32* s, wchar32 c, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == c) return const_cast<wchar32*>(s + i);
  }
  return nullptr;
}

int BIONIC_SYSV Wmemcmp(const wchar32* a, const wchar32* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

int BIONIC_SYSV Wcscoll(const wchar32* a, const wchar32* b, void*) {
  for (;; ++a, ++b) {
    if (*a != *b) return *a < *b ? -1 : 1;
    if (*a == 0) return 0;
  }
}

size_t BIONIC_SYSV Wcsxfrm(wchar32* dst, const wchar32* src, size_t n, void*) {
  size_t len = Wcslen(src);
  if (n > len) std::memcpy(dst, src, (len + 1) * sizeof(wchar32));
  return len;
}

int BIONIC_SYSV Strcoll(const char* a, const char* b, void*) { return std::strcmp(a, b); }

size_t BIONIC_SYSV Strxfrm(char* dst, const char* src, size_t n, void*) {
  size_t len = std::strlen(src);
  if (n > len) std::memcpy(dst, src, len + 1);
  return len;
}

size_t BIONIC_SYSV Strftime(char* s, size_t max, const char* fmt, const void* tm, void*) {
  // bionic's struct tm starts with the same nine ints as the CRT's.
  return strftime(s, max, fmt, static_cast<const struct tm*>(tm));
}

int64_t BIONIC_SYSV Strtoll(const char* s, char** end, int base, void*) {
  return strtoll(s, end, base);
}
uint64_t BIONIC_SYSV Strtoull(const char* s, char** end, int base, void*) {
  return strtoull(s, end, base);
}

// binary128, returned in %xmm0.
typedef int32_t Quad __attribute__((vector_size(16)));

Quad ToQuad(double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  uint64_t sign = bits >> 63;
  int64_t exp = static_cast<int64_t>((bits >> 52) & 0x7ff);
  uint64_t mant = bits & ((uint64_t{1} << 52) - 1);
  uint64_t qexp;
  if (exp == 0x7ff) {
    qexp = 0x7fff;
  } else if (exp == 0) {
    if (mant == 0) {
      qexp = 0;
    } else {  // subnormal: normalize
      int64_t e = 1 - 1023;
      while ((mant & (uint64_t{1} << 52)) == 0) {
        mant <<= 1;
        --e;
      }
      mant &= (uint64_t{1} << 52) - 1;
      qexp = static_cast<uint64_t>(e + 16383);
    }
  } else {
    qexp = static_cast<uint64_t>(exp - 1023 + 16383);
  }
  uint64_t hi = (sign << 63) | (qexp << 48) | (mant >> 4);
  uint64_t lo = mant << 60;
  Quad q;
  std::memcpy(&q, &lo, 8);
  std::memcpy(reinterpret_cast<char*>(&q) + 8, &hi, 8);
  return q;
}

Quad BIONIC_SYSV Strtold(const char* s, char** end) { return ToQuad(strtod(s, end)); }
Quad BIONIC_SYSV StrtoldL(const char* s, char** end, void*) { return ToQuad(strtod(s, end)); }

// The wide string to number functions: the digits are ASCII, so the string is
// narrowed (anything else stops the parse) and parsed by the narrow function.
template <class R, class F>
R WideParse(const wchar32* s, wchar32** end, F parse) {
  char buf[256];
  size_t n = 0;
  while (n + 1 < sizeof(buf) && s[n] != 0) {
    buf[n] = s[n] < 0x80 ? static_cast<char>(s[n]) : '\x01';
    ++n;
  }
  buf[n] = '\0';
  char* e = buf;
  R r = parse(buf, &e);
  if (end != nullptr) *end = const_cast<wchar32*>(s + (e - buf));
  return r;
}

int64_t BIONIC_SYSV Wcstol(const wchar32* s, wchar32** end, int base) {
  return WideParse<int64_t>(s, end, [base](const char* b, char** e) { return strtoll(b, e, base); });
}
uint64_t BIONIC_SYSV Wcstoul(const wchar32* s, wchar32** end, int base) {
  return WideParse<uint64_t>(s, end,
                             [base](const char* b, char** e) { return strtoull(b, e, base); });
}
double BIONIC_SYSV Wcstod(const wchar32* s, wchar32** end) {
  return WideParse<double>(s, end, [](const char* b, char** e) { return strtod(b, e); });
}
float BIONIC_SYSV Wcstof(const wchar32* s, wchar32** end) {
  return WideParse<float>(s, end, [](const char* b, char** e) { return strtof(b, e); });
}
Quad BIONIC_SYSV Wcstold(const wchar32* s, wchar32** end) {
  return ToQuad(WideParse<double>(s, end, [](const char* b, char** e) { return strtod(b, e); }));
}

void BIONIC_SYSV Openlog(const char*, int, int) {}
void BIONIC_SYSV Closelog() {}

}  // namespace

void RegisterLocale(SymbolTable& t) {
#define L(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  L("newlocale", Newlocale);
  L("freelocale", Freelocale);
  L("duplocale", Duplocale);
  L("uselocale", Uselocale);
  L("__ctype_get_mb_cur_max", CtypeGetMbCurMax);
  L("setlocale", Setlocale);
  L("localeconv", Localeconv);
  L("mbrtowc", MbrtowcSysv);
  L("mbrlen", Mbrlen);
  L("mbtowc", Mbtowc);
  L("wcrtomb", Wcrtomb);
  L("btowc", Btowc);
  L("wctob", Wctob);
  L("mbsnrtowcs", Mbsnrtowcs);
  L("mbsrtowcs", Mbsrtowcs);
  L("wcsnrtombs", Wcsnrtombs);
  L("iswalpha_l", Iswalpha);
  L("iswblank_l", Iswblank);
  L("iswcntrl_l", Iswcntrl);
  L("iswdigit_l", Iswdigit);
  L("iswlower_l", Iswlower);
  L("iswupper_l", Iswupper);
  L("iswprint_l", Iswprint);
  L("iswpunct_l", Iswpunct);
  L("iswspace_l", Iswspace);
  L("iswxdigit_l", Iswxdigit);
  L("towlower_l", Towlower);
  L("towupper_l", Towupper);
  L("wcslen", Wcslen);
  L("wmemchr", Wmemchr);
  L("wmemcmp", Wmemcmp);
  L("wcscoll_l", Wcscoll);
  L("wcsxfrm_l", Wcsxfrm);
  L("strcoll_l", Strcoll);
  L("strxfrm_l", Strxfrm);
  L("strftime_l", Strftime);
  L("strtoll_l", Strtoll);
  L("strtoull_l", Strtoull);
  L("strtold", Strtold);
  L("strtold_l", StrtoldL);
  L("wcstol", Wcstol);
  L("wcstoll", Wcstol);
  L("wcstoul", Wcstoul);
  L("wcstoull", Wcstoul);
  L("wcstod", Wcstod);
  L("wcstof", Wcstof);
  L("wcstold", Wcstold);
  L("openlog", Openlog);
  L("closelog", Closelog);
#undef L
}

}  // namespace gesher::bionic
