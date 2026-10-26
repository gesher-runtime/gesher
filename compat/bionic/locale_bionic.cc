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
#include "unicode_bionic.h"

#include <gesher/bionic_shim.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

namespace gesher::bionic {

namespace {

using wchar32 = uint32_t;
using wint32 = uint32_t;
constexpr wint32 kWEOF = 0xffffffffu;
constexpr int kEILSEQ = 84;  // Linux
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

BionicLocale* AllocateLocale(size_t mb_cur_max) {
  auto* locale = static_cast<BionicLocale*>(std::malloc(sizeof(BionicLocale)));
  if (locale != nullptr) locale->mb_cur_max = mb_cur_max;
  return locale;
}

BionicLocale* BIONIC_SYSV Newlocale(int mask, const char* name, BionicLocale* /*base*/) {
  // Android's LC_ALL (6) is a category, not a bit in LC_ALL_MASK.
  if ((mask & ~0x1fbf) != 0 || name == nullptr) {
    errno = kEINVAL;
    return nullptr;
  }
  if (!IsSupported(name)) {
    errno = kENOENT;
    return nullptr;
  }
  return AllocateLocale(IsUtf8Name(name) ? 4 : 1);
}

void BIONIC_SYSV Freelocale(BionicLocale* l) { std::free(l); }

BionicLocale* BIONIC_SYSV Duplocale(BionicLocale* l) {
  return AllocateLocale((l == kGlobalLocale ? g_global.load() : l)->mb_cur_max);
}

BionicLocale* BIONIC_SYSV Uselocale(BionicLocale* l) {
  BionicLocale* old = t_locale != nullptr ? t_locale : kGlobalLocale;
  if (l != nullptr) t_locale = l == kGlobalLocale ? nullptr : l;
  return old;
}

size_t BIONIC_SYSV CtypeGetMbCurMax() {
  return (t_locale != nullptr ? t_locale : g_global.load())->mb_cur_max;
}

char* BIONIC_SYSV Setlocale(int category, const char* name) {
  static char utf8[] = "C.UTF-8";
  static char c[] = "C";
  if (category < 0 || category > 12) {
    errno = kEINVAL;
    return nullptr;
  }
  if (name != nullptr) {
    if (!IsSupported(name)) {
      errno = kENOENT;
      return nullptr;
    }
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
  uint8_t reserved[4];  // bionic does not read or modify these bytes
};
static_assert(sizeof(BionicMbstate) == 8);

bool Initial(const BionicMbstate* ps) {
  return ps->seq[0] == 0 && ps->seq[1] == 0 && ps->seq[2] == 0 && ps->seq[3] == 0;
}

void Reset(BionicMbstate* ps) { std::memset(ps->seq, 0, sizeof(ps->seq)); }

int SequenceLength(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return -1;
}

size_t Fail(BionicMbstate* ps, int error = kEILSEQ) {
  Reset(ps);
  errno = error;
  return kInvalid;
}

size_t Mbrtowc(wchar32* pwc, const char* s, size_t n, BionicMbstate* ps) {
  static BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  if (ps->seq[3] != 0) return Fail(ps, kEINVAL);
  if (s == nullptr) {
    s = "";
    n = 1;
    pwc = nullptr;
  }
  if (n == 0) return kIncomplete;
  size_t have = ps->seq[2] != 0 ? 3 : ps->seq[1] != 0 ? 2 : ps->seq[0] != 0 ? 1 : 0;
  size_t used = 0;
  if (have == 0 && static_cast<uint8_t>(*s) < 0x80) {
    if (pwc != nullptr) *pwc = static_cast<uint8_t>(*s);
    return *s == 0 ? 0 : 1;
  }
  int len = SequenceLength(have == 0 ? static_cast<uint8_t>(*s) : ps->seq[0]);
  if (len < 2) return Fail(ps);
  while (have < static_cast<size_t>(len) && used < n) {
    uint8_t b = static_cast<uint8_t>(s[used++]);
    if (have != 0 && (b & 0xC0) != 0x80) return Fail(ps);
    ps->seq[have++] = b;
  }
  if (have < static_cast<size_t>(len)) return kIncomplete;
  wchar32 c = ps->seq[0] & (0x7fu >> len);
  for (int i = 1; i < len; ++i) c = (c << 6) | (ps->seq[i] & 0x3fu);
  static const wchar32 kMin[] = {0, 0, 0x80, 0x800, 0x10000};
  if (c < kMin[len] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return Fail(ps);
  Reset(ps);
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
  if (c < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (c >> 12));
    buf[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (c & 0x3F));
    return 3;
  }
  // Preserve the pinned c32rtomb encoder's asymmetric acceptance: decoding
  // rejects surrogates and values above U+10FFFF; encoding accepts <= 0x1FFFFF.
  if (c <= 0x1FFFFF) {
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
  static BionicMbstate internal;
  return Mbrtowc(nullptr, s, n, ps != nullptr ? ps : &internal);
}

int BIONIC_SYSV Mbtowc(wchar32* pwc, const char* s, size_t n) {
  static BionicMbstate st{};
  if (s == nullptr) {
    std::memset(&st, 0, sizeof(st));
    return 0;
  }
  size_t r = Mbrtowc(pwc, s, n, &st);
  if (r == kInvalid || r == kIncomplete) {
    errno = kEILSEQ;
    return -1;
  }
  return static_cast<int>(r);
}

size_t BIONIC_SYSV Wcrtomb(char* s, wchar32 wc, BionicMbstate* ps) {
  static BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  if (s == nullptr || wc == 0) {
    if (s != nullptr) *s = 0;
    Reset(ps);
    return 1;
  }
  if (!Initial(ps)) return Fail(ps);
  size_t r = Encode(wc, s);
  if (r == kInvalid) errno = kEILSEQ;
  return r;
}

size_t BIONIC_SYSV Mbrtoc32(wchar32* pc, const char* s, size_t n, BionicMbstate* ps) {
  static BionicMbstate internal;
  return Mbrtowc(pc, s, n, ps != nullptr ? ps : &internal);
}

size_t BIONIC_SYSV Mbrtoc16(uint16_t* pc, const char* s, size_t n, BionicMbstate* ps) {
  static BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  uint16_t ignored;
  if (pc == nullptr) pc = &ignored;
  if (ps->seq[3] != 0) {
    *pc = static_cast<uint16_t>((ps->seq[1] << 8) | ps->seq[0]);
    Reset(ps);
    return static_cast<size_t>(-3);
  }
  wchar32 c;
  size_t result = Mbrtowc(&c, s, n, ps);
  if (result == kInvalid || result == kIncomplete) return result;
  // The pinned implementation returns zero without writing pc16 for NUL.
  if (result == 0) return 0;
  if (c < 0x10000) {
    *pc = static_cast<uint16_t>(c);
  } else {
    c -= 0x10000;
    uint16_t trail = static_cast<uint16_t>((c & 0x3ff) | 0xdc00);
    ps->seq[0] = static_cast<uint8_t>(trail);
    ps->seq[1] = static_cast<uint8_t>(trail >> 8);
    ps->seq[3] = static_cast<uint8_t>(result);
    *pc = static_cast<uint16_t>((c >> 10) | 0xd800);
  }
  return result;
}

size_t BIONIC_SYSV C32rtomb(char* s, wchar32 c, BionicMbstate* ps) {
  static BionicMbstate internal;
  return Wcrtomb(s, c, ps != nullptr ? ps : &internal);
}

size_t BIONIC_SYSV C16rtomb(char* s, uint16_t c, BionicMbstate* ps) {
  static BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  bool low = c >= 0xdc00 && c < 0xe000;
  if (Initial(ps)) {
    if (c >= 0xd800 && c < 0xdc00) {
      // Match bionic's byte representation, including its U+D800 quirk:
      // its zero payload leaves the state initial, so the next low fails.
      wchar32 payload = (c & 0x3ffu) << 10;
      ps->seq[2] = static_cast<uint8_t>(payload >> 8);
      ps->seq[3] = static_cast<uint8_t>(payload >> 16);
      return 0;
    }
    if (low) return Fail(ps, kEINVAL);
    return Wcrtomb(s, c, ps);
  }
  if (!low) return Fail(ps, kEINVAL);
  wchar32 scalar = ((ps->seq[3] << 16) | (ps->seq[2] << 8) | (c & 0x3ffu)) + 0x10000;
  size_t result = Wcrtomb(s, scalar, nullptr);
  Reset(ps);
  return result;
}

int BIONIC_SYSV Mbsinit(const BionicMbstate* ps) { return ps == nullptr || Initial(ps); }

wint32 BIONIC_SYSV Btowc(int c) {
  return c != EOF && static_cast<unsigned>(c) < 0x80 ? static_cast<wint32>(c) : kWEOF;
}

int BIONIC_SYSV Wctob(wint32 c) { return c < 0x80 ? static_cast<int>(c) : EOF; }

size_t BIONIC_SYSV Mbsnrtowcs(wchar32* dst, const char** src, size_t nms, size_t len,
                              BionicMbstate* ps) {
  static BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  const char* s = *src;
  // bionic checks pending/ASCII before considering the output bound.
  if (nms > 0 && (ps->seq[0] != 0 || ps->seq[1] != 0 || ps->seq[2] != 0) &&
      static_cast<uint8_t>(*s) < 0x80) return Fail(ps);
  size_t count = 0;
  while (dst == nullptr || count < len) {
    if (nms == 0) break;
    wchar32 c;
    size_t r;
    if (static_cast<uint8_t>(*s) < 0x80) {
      c = static_cast<uint8_t>(*s);
      r = c == 0 ? 0 : 1;
    } else {
      r = Mbrtowc(&c, s, nms, ps);
    }
    if (r == kInvalid) {
      if (dst != nullptr) *src = s;
      return Fail(ps);
    }
    if (r == kIncomplete) {
      if (dst != nullptr) *src = s + nms;
      return Fail(ps);
    }
    if (r == 0) {
      if (dst != nullptr) {
        dst[count] = 0;
        *src = nullptr;
      }
      Reset(ps);
      return count;
    }
    if (dst != nullptr) dst[count] = c;
    s += r;
    nms -= r;
    ++count;
  }
  if (dst != nullptr) *src = s;
  Reset(ps);
  return count;
}

size_t BIONIC_SYSV Mbsrtowcs(wchar32* dst, const char** src, size_t len, BionicMbstate* ps) {
  return Mbsnrtowcs(dst, src, SIZE_MAX, len, ps);
}

size_t BIONIC_SYSV Wcsnrtombs(char* dst, const wchar32** src, size_t nwc, size_t len,
                              BionicMbstate* ps) {
  static BionicMbstate internal;
  if (ps == nullptr) ps = &internal;
  if (!Initial(ps)) return Fail(ps);
  const wchar32* w = *src;
  size_t count = 0;
  char buf[4];
  for (; nwc > 0 && (dst == nullptr || count < len); ++w, --nwc) {
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
      if (r > len - count) break;
      std::memcpy(dst + count, buf, r);
    }
    count += r;
  }
  if (dst != nullptr) *src = w;
  return count;
}

size_t BIONIC_SYSV Wcsrtombs(char* dst, const wchar32** src, size_t len, BionicMbstate* ps) {
  return Wcsnrtombs(dst, src, SIZE_MAX, len, ps);
}

size_t BIONIC_SYSV Mbstowcs(wchar32* dst, const char* src, size_t len) {
  BionicMbstate state{};
  return Mbsrtowcs(dst, &src, len, &state);
}

size_t BIONIC_SYSV Wcstombs(char* dst, const wchar32* src, size_t len) {
  BionicMbstate state{};
  return Wcsrtombs(dst, &src, len, &state);
}

int BIONIC_SYSV Wctomb(char* dst, wchar32 c) {
  static BionicMbstate state{};
  if (dst == nullptr) {
    std::memset(&state, 0, sizeof(state));
    return 0;
  }
  return static_cast<int>(Wcrtomb(dst, c, &state));
}

int BIONIC_SYSV Mblen(const char* src, size_t len) {
  BionicMbstate state{};
  // The pinned mblen returns the mbrlen result as int, including -2.
  return static_cast<int>(Mbrlen(src, len, &state));
}

// --- wide characters ----------------------------------------------------------------

#define WCLASS_L(name, property) \
  int BIONIC_SYSV name(wint32 c, void*) { return UnicodeValue(c, UnicodeProperty::property); }
WCLASS_L(Iswalnum, Alnum) WCLASS_L(Iswalpha, Alpha) WCLASS_L(Iswblank, Blank)
WCLASS_L(Iswcntrl, Cntrl) WCLASS_L(Iswdigit, Digit) WCLASS_L(Iswgraph, Graph)
WCLASS_L(Iswlower, Lower) WCLASS_L(Iswprint, Print) WCLASS_L(Iswpunct, Punct)
WCLASS_L(Iswspace, Space) WCLASS_L(Iswupper, Upper) WCLASS_L(Iswxdigit, Xdigit)
#undef WCLASS_L
wint32 BIONIC_SYSV Towlower(wint32 c, void*) { return UnicodeValue(c, UnicodeProperty::ToLower); }
wint32 BIONIC_SYSV Towupper(wint32 c, void*) { return UnicodeValue(c, UnicodeProperty::ToUpper); }

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
  // Android aliases this to wcslcpy, including its terminating NUL when cut.
  if (n != 0) {
    size_t copied = len < n - 1 ? len : n - 1;
    std::memcpy(dst, src, copied * sizeof(wchar32));
    dst[copied] = 0;
  }
  return len;
}

int BIONIC_SYSV Strcoll(const char* a, const char* b, void*) { return std::strcmp(a, b); }

size_t BIONIC_SYSV Strxfrm(char* dst, const char* src, size_t n, void*) {
  size_t len = std::strlen(src);
  if (n > len) std::memcpy(dst, src, len + 1);
  return len;
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

// Floating conversions use the same candidate alphabet as bionic/wcstod.cpp,
// after its Unicode whitespace skip. Keep full input lengths/end pointers.
template <class R, class F>
R WideParse(const wchar32* s, wchar32** end, F parse) {
  const wchar32* start = s;
  while (UnicodeValue(*start, UnicodeProperty::Space)) ++start;
  std::string ascii;
  constexpr char alphabet[] = "-+0123456789.xXeEpP()nNaAiIfFtTyY";
  for (const wchar32* p = start; *p != 0 && *p < 0x80 &&
                               std::strchr(alphabet, static_cast<char>(*p)) != nullptr; ++p) {
    ascii += static_cast<char>(*p);
  }
  char* e;
  R r = parse(ascii.c_str(), &e);
  if (end != nullptr) *end = const_cast<wchar32*>(e == ascii.c_str() ? s : start + (e - ascii.c_str()));
  return r;
}

int Digit(wchar32 c) {
  if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
  if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A' + 10);
  if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a' + 10);
  return -1;
}

template <bool Signed>
uint64_t WideInteger(const wchar32* s, wchar32** end, int base) {
  if (end != nullptr) *end = const_cast<wchar32*>(s);
  if (base < 0 || base == 1 || base > 36) {
    errno = kEINVAL;
    return 0;
  }
  const wchar32* p = s;
  // The pinned generic StrToI uses narrow isspace even for wchar_t.
  while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
  bool negative = *p == '-';
  if (*p == '-' || *p == '+') ++p;
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
      Digit(p[2]) >= 0 && Digit(p[2]) < 16) {
    p += 2;
    base = 16;
  }
  // Pinned bionic tests isdigit, rather than < 2, before accepting 0b.
  if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B') &&
      p[2] >= '0' && p[2] <= '9') {
    p += 2;
    base = 2;
  }
  if (base == 0) base = *p == '0' ? 8 : 10;
  uint64_t limit = Signed ? (negative ? uint64_t{1} << 63 : INT64_MAX) : UINT64_MAX;
  uint64_t value = 0;
  bool any = false;
  bool overflow = false;
  for (int digit; (digit = Digit(*p)) >= 0 && digit < base; ++p) {
    any = true;
    if (value > (limit - static_cast<unsigned>(digit)) / static_cast<unsigned>(base)) {
      overflow = true;
    } else if (!overflow) {
      value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
    }
  }
  if (any && end != nullptr) *end = const_cast<wchar32*>(p);
  if (overflow) {
    errno = kERANGE;
    return limit;
  }
  return negative ? uint64_t{0} - value : value;
}

int64_t BIONIC_SYSV Wcstol(const wchar32* s, wchar32** end, int base) {
  return static_cast<int64_t>(WideInteger<true>(s, end, base));
}
uint64_t BIONIC_SYSV Wcstoul(const wchar32* s, wchar32** end, int base) {
  return WideInteger<false>(s, end, base);
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

int64_t BIONIC_SYSV WcstolL(const wchar32* s, wchar32** end, int base, void*) {
  return Wcstol(s, end, base);
}
uint64_t BIONIC_SYSV WcstoulL(const wchar32* s, wchar32** end, int base, void*) {
  return Wcstoul(s, end, base);
}
double BIONIC_SYSV WcstodL(const wchar32* s, wchar32** end, void*) { return Wcstod(s, end); }
float BIONIC_SYSV WcstofL(const wchar32* s, wchar32** end, void*) { return Wcstof(s, end); }

size_t BIONIC_SYSV Wcsftime(wchar32* dst, size_t max, const wchar32* format, const void* tm,
                           void*) {
  if (max >= SIZE_MAX / CtypeGetMbCurMax()) {
    errno = kEINVAL;
    return 0;
  }
  using Buffer = std::unique_ptr<char, decltype(&std::free)>;
  const wchar32* fp = format;
  BionicMbstate state{};
  size_t bytes = Wcsrtombs(nullptr, &fp, 0, &state);
  if (bytes == kInvalid) return 0;
  Buffer narrow_format(static_cast<char*>(std::malloc(bytes + 1)), std::free);
  Buffer output(static_cast<char*>(std::malloc(max * CtypeGetMbCurMax())), std::free);
  if (!narrow_format || !output) return 0;
  Wcsrtombs(narrow_format.get(), &fp, bytes + 1, &state);
  using Format = size_t(BIONIC_SYSV*)(char*, size_t, const char*, const void*);
  auto strftime = reinterpret_cast<Format>(LookupBionicSymbol("strftime"));
  // Pinned FreeBSD wcsftime allocates max*MB_CUR_MAX but passes max bytes to
  // strftime. Preserve that byte bound, even when UTF-32 output would fit.
  if (strftime(output.get(), max, narrow_format.get(), tm) == 0) return 0;
  const char* source = output.get();
  size_t result = Mbsrtowcs(dst, &source, max, &state);
  return result == kInvalid || result == kIncomplete || source != nullptr ? 0 : result;
}

void BIONIC_SYSV Openlog(const char*, int, int) {}
void BIONIC_SYSV Closelog() {}

}  // namespace

void DoubleToBinary128(double d, void* out) {
  Quad q = ToQuad(d);
  std::memcpy(out, &q, sizeof(q));
}

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
  L("mbrtoc16", Mbrtoc16);
  L("mbrtoc32", Mbrtoc32);
  L("c16rtomb", C16rtomb);
  L("c32rtomb", C32rtomb);
  L("mbsinit", Mbsinit);
  L("mbrlen", Mbrlen);
  L("mblen", Mblen);
  L("mbtowc", Mbtowc);
  L("wctomb", Wctomb);
  L("mbstowcs", Mbstowcs);
  L("wcstombs", Wcstombs);
  L("wcrtomb", Wcrtomb);
  L("btowc", Btowc);
  L("wctob", Wctob);
  L("mbsnrtowcs", Mbsnrtowcs);
  L("mbsrtowcs", Mbsrtowcs);
  L("mbsrtowcs_l", Mbsrtowcs);
  L("wcsnrtombs", Wcsnrtombs);
  L("wcsrtombs", Wcsrtombs);
  L("wcsrtombs_l", Wcsrtombs);
  L("iswalnum_l", Iswalnum);
  L("iswalpha_l", Iswalpha);
  L("iswblank_l", Iswblank);
  L("iswcntrl_l", Iswcntrl);
  L("iswdigit_l", Iswdigit);
  L("iswgraph_l", Iswgraph);
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
  L("wcscoll", Wcscoll);
  L("wcsxfrm_l", Wcsxfrm);
  L("wcsxfrm", Wcsxfrm);
  L("strcoll_l", Strcoll);
  L("strxfrm_l", Strxfrm);
  L("strtoll_l", Strtoll);
  L("strtoull_l", Strtoull);
  L("strtold", Strtold);
  L("strtold_l", StrtoldL);
  L("wcstol", Wcstol);
  L("wcstoll", Wcstol);
  L("wcstoimax", Wcstol);
  L("wcstol_l", WcstolL);
  L("wcstoll_l", WcstolL);
  L("wcstoul", Wcstoul);
  L("wcstoull", Wcstoul);
  L("wcstoumax", Wcstoul);
  L("wcstoul_l", WcstoulL);
  L("wcstoull_l", WcstoulL);
  L("wcstod", Wcstod);
  L("wcstod_l", WcstodL);
  L("wcstof", Wcstof);
  L("wcstof_l", WcstofL);
  L("wcstold", Wcstold);
  L("wcsftime", Wcsftime);
  L("wcsftime_l", Wcsftime);
  L("openlog", Openlog);
  L("closelog", Closelog);
#undef L
}

}  // namespace gesher::bionic
