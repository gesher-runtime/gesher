// Environment declarations only; the original pinned bodies follow verbatim.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "native3_wide_contracts.h"

#define __strong_alias(a, b)
#define DEF_STRONG(a)
#undef __BEGIN_DECLS
#undef __END_DECLS
#define __BEGIN_DECLS
#define __END_DECLS
#undef __nodiscard
#define __nodiscard
#undef _tolower
#define BIONIC_MULTIBYTE_RESULT_ILLEGAL_SEQUENCE static_cast<size_t>(-1)
#define BIONIC_MULTIBYTE_RESULT_INCOMPLETE_SEQUENCE static_cast<size_t>(-2)
#define MIN(a, b) std::min(a, b)

namespace pinned {
struct mbstate_t { unsigned char __seq[4]; unsigned char __reserved[4]; };
size_t mbrtoc32(char32_t*, const char*, size_t, mbstate_t*);
size_t c32rtomb(char*, char32_t, mbstate_t*);
size_t mbrtowc(wchar_t*, const char*, size_t, mbstate_t*);
size_t wcrtomb(char*, wchar_t, mbstate_t*);
size_t mbrlen(const char*, size_t, mbstate_t*);
size_t mbsrtowcs(wchar_t*, const char**, size_t, mbstate_t*);
size_t wcsrtombs(char*, const wchar_t**, size_t, mbstate_t*);

// bionic's ctype predicates reject non-ASCII wchar values (strtol.cpp uses
// these narrow predicates even in its wchar_t instantiation).
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int _tolower(int c) { return c | 0x20; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }

// The original wcstod/parsefloat bodies use only this finite input stream.
// Numeric rounding is supplied by host strtod/strtof, so this extension
// verifies the pinned wide grammar/end handling, not bionic's gdtoa engine.
struct FILE {
  int _flags;
  struct { unsigned char* _base; size_t _size; } _bf;
  unsigned char* _p;
  size_t _r;
  int (*_read)(void*, char*, int);
};
struct __sfileext {};
#define _FILEEXT_SETUP(f, e) static_cast<void>(e)
#define __SRD 1
int __srefill(FILE*) { return 1; }
int ungetc(int c, FILE*) { return c; }
int iswspace(wchar_t c) { return isspace(c) || c == 0x2003; }
