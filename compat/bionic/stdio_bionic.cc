// gesher bionic shim: stdio, the printf/scanf family and liblog.
//
// The variadic entry points are the hard part. The .so calls printf(fmt, ...)
// with the System V convention: the fixed and variable arguments in rdi, rsi,
// rdx, rcx, r8, r9 and xmm0-7, the rest on the stack. A sysv_abi C++ function
// cannot use va_start on Windows (docs/decisions/0011-libcore-natives.md), so
// each variadic entry point is a small assembly trampoline that spills the
// argument registers into a System V register save area, builds a System V
// va_list over it (gp_offset, fp_offset, overflow_arg_area, reg_save_area)
// and calls the va_list form of the function (vprintf, ...). Those take the
// va_list the way the .so passes one, a pointer to that structure, so the .so
// can call vsnprintf & co. directly too.
//
// The formatter walks the format itself, takes each argument from the System
// V va_list by its conversion, and formats one conversion at a time with the
// CRT, mapping bionic's LP64 lengths (`%ld` is 64-bit there) and its
// extensions (`%m`, `%p` as 0x...). The scanf family passes the pointers on to
// the CRT's sscanf with the format's lengths mapped the same way.

#include "shim_internal.h"

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gesher::bionic {

namespace {

// bionic's x86-64 long double is IEEE binary128, passed like an SSE value of
// 16 bytes. Narrowed to double for the CRT (which has no binary128).
double NextFp128(SysvVaList* ap) {
  uint64_t lo, hi;
  if (ap->fp_offset < 176) {
    std::memcpy(&lo, ap->reg_save_area + ap->fp_offset, 8);
    std::memcpy(&hi, ap->reg_save_area + ap->fp_offset + 8, 8);
    ap->fp_offset += 16;
  } else {
    auto a = reinterpret_cast<uintptr_t>(ap->overflow_arg_area);
    a = (a + 15) & ~uintptr_t{15};
    std::memcpy(&lo, reinterpret_cast<void*>(a), 8);
    std::memcpy(&hi, reinterpret_cast<void*>(a + 8), 8);
    ap->overflow_arg_area = reinterpret_cast<uint8_t*>(a + 16);
  }
  uint64_t sign = hi >> 63;
  int64_t exp = static_cast<int64_t>((hi >> 48) & 0x7fff);
  uint64_t mant = ((hi & 0xffffffffffffULL) << 4) | (lo >> 60);
  uint64_t bits;
  if (exp == 0x7fff) {
    bits = (sign << 63) | (0x7ffULL << 52) | mant;
  } else if (exp == 0) {
    bits = sign << 63;
  } else {
    int64_t e = exp - 16383 + 1023;
    if (e >= 0x7ff) {
      bits = (sign << 63) | (0x7ffULL << 52);
    } else if (e <= 0) {
      bits = sign << 63;
    } else {
      bits = (sign << 63) | (static_cast<uint64_t>(e) << 52) | mant;
    }
  }
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

// --- formatting ------------------------------------------------------------

// Appends one CRT-formatted conversion.
void AppendF(std::string* out, const char* spec, ...) {
  char small[128];
  va_list ap;
  va_start(ap, spec);
  int n = std::vsnprintf(small, sizeof(small), spec, ap);
  va_end(ap);
  if (n < 0) return;
  if (static_cast<size_t>(n) < sizeof(small)) {
    out->append(small, static_cast<size_t>(n));
    return;
  }
  std::string big(static_cast<size_t>(n) + 1, '\0');
  va_start(ap, spec);
  std::vsnprintf(big.data(), big.size(), spec, ap);
  va_end(ap);
  out->append(big.data(), static_cast<size_t>(n));
}

// A UTF-32 (bionic wchar_t) string as UTF-8.
std::string Utf32ToUtf8(const uint32_t* s, int max_units) {
  std::string out;
  for (int i = 0; s[i] != 0 && (max_units < 0 || i < max_units); ++i) {
    uint32_t c = s[i];
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

// UTF-8 as UTF-32 (bionic wchar_t); an invalid sequence gives U+FFFD.
std::vector<uint32_t> Utf8ToUtf32(const std::string& s) {
  std::vector<uint32_t> out;
  size_t i = 0;
  while (i < s.size()) {
    auto b = static_cast<uint8_t>(s[i]);
    int extra = b < 0x80 ? 0 : (b & 0xe0) == 0xc0 ? 1 : (b & 0xf0) == 0xe0 ? 2 : (b & 0xf8) == 0xf0 ? 3 : -1;
    uint32_t c = extra <= 0 ? b : extra == 1 ? (b & 0x1fu) : extra == 2 ? (b & 0x0fu) : (b & 0x07u);
    bool ok = extra >= 0 && i + static_cast<size_t>(extra) < s.size();
    for (int k = 1; ok && k <= extra; ++k) {
      auto nb = static_cast<uint8_t>(s[i + static_cast<size_t>(k)]);
      if ((nb & 0xc0) != 0x80) ok = false;
      c = (c << 6) | (nb & 0x3fu);
    }
    if (!ok) {
      out.push_back(0xfffd);
      ++i;
      continue;
    }
    out.push_back(c);
    i += static_cast<size_t>(extra) + 1;
  }
  return out;
}

enum class Len { kNone, kHH, kH, kL, kLL, kBigL, kJ, kZ, kT };

std::string Format(const char* fmt, SysvVaList* ap) {
  std::string out;
  const char* p = fmt;
  while (*p != 0) {
    if (*p != '%') {
      const char* q = std::strchr(p, '%');
      size_t n = q ? static_cast<size_t>(q - p) : std::strlen(p);
      out.append(p, n);
      p += n;
      continue;
    }
    const char* start = p++;
    if (*p == '%') {
      out += '%';
      ++p;
      continue;
    }
    std::string flags;
    while (*p != 0 && std::strchr("-+ #0'", *p) != nullptr) {
      if (*p != '\'') flags += *p;  // the CRT has no grouping flag
      ++p;
    }
    int width = -1;
    if (*p == '*') {
      width = static_cast<int>(NextGp(ap));
      if (width < 0) {
        flags += '-';
        width = -width;
      }
      ++p;
    } else {
      while (*p >= '0' && *p <= '9') width = (width < 0 ? 0 : width * 10) + (*p++ - '0');
    }
    int prec = -1;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        prec = static_cast<int>(NextGp(ap));
        if (prec < 0) prec = -1;
        ++p;
      } else {
        prec = 0;
        while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
      }
    }
    Len len = Len::kNone;
    switch (*p) {
      case 'h':
        ++p;
        len = Len::kH;
        if (*p == 'h') {
          ++p;
          len = Len::kHH;
        }
        break;
      case 'l':
        ++p;
        len = Len::kL;
        if (*p == 'l') {
          ++p;
          len = Len::kLL;
        }
        break;
      case 'q': ++p; len = Len::kLL; break;
      case 'L': ++p; len = Len::kBigL; break;
      case 'j': ++p; len = Len::kJ; break;
      case 'z': ++p; len = Len::kZ; break;
      case 't': ++p; len = Len::kT; break;
      default: break;
    }
    char conv = *p;
    if (conv == 0) {
      out.append(start);
      break;
    }
    ++p;

    std::string spec = "%" + flags;
    if (width >= 0) spec += std::to_string(width);
    std::string spec_no_prec = spec;
    if (prec >= 0) spec += "." + std::to_string(prec);

    switch (conv) {
      case 'd':
      case 'i': {
        uint64_t raw = NextGp(ap);
        long long v;
        switch (len) {
          case Len::kHH: v = static_cast<signed char>(raw); break;
          case Len::kH: v = static_cast<short>(raw); break;
          case Len::kNone: v = static_cast<int>(raw); break;
          default: v = static_cast<long long>(raw); break;
        }
        AppendF(&out, (spec + "lld").c_str(), v);
        break;
      }
      case 'u':
      case 'o':
      case 'x':
      case 'X': {
        uint64_t raw = NextGp(ap);
        unsigned long long v;
        switch (len) {
          case Len::kHH: v = static_cast<unsigned char>(raw); break;
          case Len::kH: v = static_cast<unsigned short>(raw); break;
          case Len::kNone: v = static_cast<unsigned int>(raw); break;
          default: v = raw; break;
        }
        AppendF(&out, (spec + "ll" + conv).c_str(), v);
        break;
      }
      case 'c': {
        uint64_t raw = NextGp(ap);
        if (len == Len::kL) {
          uint32_t wc[2] = {static_cast<uint32_t>(raw), 0};
          AppendF(&out, (spec_no_prec + "s").c_str(), Utf32ToUtf8(wc, -1).c_str());
        } else {
          AppendF(&out, (spec_no_prec + "c").c_str(), static_cast<int>(raw));
        }
        break;
      }
      case 's': {
        auto ptr = reinterpret_cast<const void*>(NextGp(ap));
        if (ptr == nullptr) {
          AppendF(&out, (spec + "s").c_str(), "(null)");
        } else if (len == Len::kL) {
          std::string s = Utf32ToUtf8(static_cast<const uint32_t*>(ptr), prec);
          AppendF(&out, (spec_no_prec + "s").c_str(), s.c_str());
        } else {
          AppendF(&out, (spec + "s").c_str(), static_cast<const char*>(ptr));
        }
        break;
      }
      case 'p': {
        uint64_t v = NextGp(ap);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
        AppendF(&out, (spec_no_prec + "s").c_str(), buf);
        break;
      }
      case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
        double v = len == Len::kBigL ? NextFp128(ap) : NextFp(ap);
        AppendF(&out, (spec + conv).c_str(), v);
        break;
      }
      case 'n': {
        void* ptr = reinterpret_cast<void*>(NextGp(ap));
        if (ptr != nullptr) {
          int64_t n = static_cast<int64_t>(out.size());
          switch (len) {
            case Len::kHH: *static_cast<signed char*>(ptr) = static_cast<signed char>(n); break;
            case Len::kH: *static_cast<short*>(ptr) = static_cast<short>(n); break;
            case Len::kNone: *static_cast<int*>(ptr) = static_cast<int>(n); break;
            default: *static_cast<int64_t*>(ptr) = n; break;
          }
        }
        break;
      }
      case 'm':  // bionic/glibc: strerror(errno), no argument
        AppendF(&out, (spec + "s").c_str(), strerror(errno));
        break;
      default:  // unknown: copy the conversion through
        out.append(start, static_cast<size_t>(p - start));
        break;
    }
  }
  return out;
}

// --- FILE ------------------------------------------------------------------
// The .so's stdin/stdout/stderr are the CRT's streams (the data symbols below).
// Libraries built for API < 23 name them as &__sF[0..2] instead, bionic's
// array of FILE structs (152 bytes each on LP64); those are mapped back.

constexpr size_t kBionicFileSize = 152;
alignas(16) uint8_t g_sF[3 * kBionicFileSize];

FILE* g_stdin = stdin;
FILE* g_stdout = stdout;
FILE* g_stderr = stderr;

FILE* F(void* f) {
  auto p = static_cast<uint8_t*>(f);
  if (p >= g_sF && p < g_sF + sizeof(g_sF)) {
    switch ((p - g_sF) / kBionicFileSize) {
      case 0: return stdin;
      case 1: return stdout;
      default: return stderr;
    }
  }
  return static_cast<FILE*>(f);
}

// bionic's fopen modes as the CRT's: always binary (bionic has no text
// mode), 'e' (O_CLOEXEC) as 'N' (not inherited), 'x' kept; anything else the
// CRT would reject as an invalid parameter is dropped.
std::string CrtMode(const char* mode) {
  std::string out;
  if (mode == nullptr || (*mode != 'r' && *mode != 'w' && *mode != 'a')) return out;
  out += *mode;
  bool plus = false, excl = false, noinherit = false;
  for (const char* p = mode + 1; *p; ++p) {
    if (*p == '+') plus = true;
    if (*p == 'x') excl = true;
    if (*p == 'e') noinherit = true;
  }
  if (plus) out += '+';
  out += 'b';
  if (excl && *mode == 'w') out += 'x';
  if (noinherit) out += 'N';
  return out;
}
// fopen through gesher-base's open (Linux O_* flags, delete access shared, so
// the file can be unlinked or renamed while open, as on Linux), then fdopen.
FILE* BIONIC_SYSV Fopen(const char* path, const char* mode) {
  std::string m = CrtMode(mode);
  if (m.empty()) return errno = kEINVAL, nullptr;
  bool plus = m.find('+') != std::string::npos;
  int flags = 0;
  switch (m[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    default: flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
  }
  if (m.find('x') != std::string::npos) flags |= O_EXCL;
  if (m.find('N') != std::string::npos) flags |= O_CLOEXEC;
  int fd = open(path, flags, 0666);
  if (fd < 0) return nullptr;
  std::string crt = m.substr(0, plus ? 2 : 1) + "b";
  FILE* f = fdopen(fd, crt.c_str());
  if (f == nullptr) {
    int saved = errno;
    close(fd);
    errno = saved;
  }
  return f;
}
FILE* BIONIC_SYSV Fdopen(int fd, const char* mode) {
  std::string m = CrtMode(mode);
  if (m.empty()) return errno = kEINVAL, nullptr;
  if (m.back() == 'N') m.pop_back();
  if (m.size() > 1 && m.back() == 'x') m.pop_back();
  return fdopen(fd, m.c_str());
}
FILE* BIONIC_SYSV Freopen(const char* path, const char* mode, void* f) {
  std::string m = CrtMode(mode);
  if (m.empty()) return errno = kEINVAL, nullptr;
  return std::freopen(path, m.c_str(), F(f));
}
int BIONIC_SYSV Fgetpos(void* f, int64_t* pos) {
  int64_t p = _ftelli64(F(f));
  if (p < 0) return -1;
  *pos = p;
  return 0;
}
int BIONIC_SYSV Fsetpos(void* f, const int64_t* pos) { return _fseeki64(F(f), *pos, SEEK_SET); }

// getline/getdelim: *line grows with the CRT's realloc, which the .so's
// free (the shim's) accepts.
int64_t BIONIC_SYSV Getdelim(char** line, size_t* n, int delim, void* f) {
  if (line == nullptr || n == nullptr) return errno = kEINVAL, -1;
  FILE* fp = F(f);
  size_t len = 0;
  int c;
  while ((c = std::fgetc(fp)) != EOF) {
    if (*line == nullptr || len + 2 > *n) {
      size_t cap = *n < 120 ? 120 : *n * 2;
      char* grown = static_cast<char*>(std::realloc(*line, cap));
      if (grown == nullptr) return errno = kENOMEM, -1;
      *line = grown;
      *n = cap;
    }
    (*line)[len++] = static_cast<char>(c);
    if (c == delim) break;
  }
  if (len == 0) return -1;
  (*line)[len] = 0;
  return static_cast<int64_t>(len);
}
int64_t BIONIC_SYSV Getline(char** line, size_t* n, void* f) { return Getdelim(line, n, '\n', f); }

// --- wide characters on a FILE: UTF-8 in the stream, UTF-32 in the .so --------

constexpr uint32_t kWeof = 0xffffffffu;
std::mutex g_unget_mutex;
std::unordered_map<FILE*, std::vector<uint32_t>> g_unget;  // ungetwc pushback

uint32_t BIONIC_SYSV Fgetwc(void* f) {
  FILE* fp = F(f);
  {
    std::lock_guard<std::mutex> lock(g_unget_mutex);
    auto it = g_unget.find(fp);
    if (it != g_unget.end() && !it->second.empty()) {
      uint32_t c = it->second.back();
      it->second.pop_back();
      return c;
    }
  }
  int b = std::fgetc(fp);
  if (b == EOF) return kWeof;
  int extra = b < 0x80 ? 0 : b < 0xe0 ? 1 : b < 0xf0 ? 2 : 3;
  uint32_t c = extra == 0 ? b : extra == 1 ? (b & 0x1f) : extra == 2 ? (b & 0x0f) : (b & 0x07);
  for (int i = 0; i < extra; ++i) {
    int nb = std::fgetc(fp);
    if (nb == EOF || (nb & 0xc0) != 0x80) {
      if (nb != EOF) std::ungetc(nb, fp);
      errno = 84;  // EILSEQ
      return kWeof;
    }
    c = (c << 6) | (nb & 0x3f);
  }
  return c;
}
uint32_t BIONIC_SYSV Ungetwc(uint32_t c, void* f) {
  if (c == kWeof) return kWeof;
  std::lock_guard<std::mutex> lock(g_unget_mutex);
  g_unget[F(f)].push_back(c);
  return c;
}
uint32_t BIONIC_SYSV Fputwc(uint32_t c, void* f) {
  uint32_t wc[2] = {c, 0};
  std::string s = c == 0 ? std::string(1, '\0') : Utf32ToUtf8(wc, -1);
  return std::fwrite(s.data(), 1, s.size(), F(f)) == s.size() ? c : kWeof;
}
int BIONIC_SYSV Fputws(const uint32_t* ws, void* f) {
  std::string s = Utf32ToUtf8(ws, -1);
  return std::fwrite(s.data(), 1, s.size(), F(f)) == s.size() ? 0 : -1;
}
uint32_t BIONIC_SYSV Getwchar() { return Fgetwc(stdin); }
uint32_t BIONIC_SYSV Putwchar(uint32_t c) { return Fputwc(c, stdout); }
int BIONIC_SYSV Fwide(void*, int mode) { return mode > 0 ? 1 : mode < 0 ? -1 : 0; }
int BIONIC_SYSV Fclose(void* f) { return std::fclose(F(f)); }
size_t BIONIC_SYSV Fread(void* buf, size_t size, size_t n, void* f) {
  return std::fread(buf, size, n, F(f));
}
size_t BIONIC_SYSV Fwrite(const void* buf, size_t size, size_t n, void* f) {
  return std::fwrite(buf, size, n, F(f));
}
int BIONIC_SYSV Fputs(const char* s, void* f) { return std::fputs(s, F(f)); }
int BIONIC_SYSV Fputc(int c, void* f) { return std::fputc(c, F(f)); }
int BIONIC_SYSV Putchar(int c) { return std::putchar(c); }
int BIONIC_SYSV Puts(const char* s) { return std::puts(s); }
char* BIONIC_SYSV Fgets(char* s, int n, void* f) { return std::fgets(s, n, F(f)); }
[[noreturn]] void StdioFortify(const char* what) {
  std::fprintf(stderr, "gesher bionic: FORTIFY: %s: prevented buffer overflow\n", what);
  std::abort();
}
char* BIONIC_SYSV FgetsChk(char* s, int n, void* f, size_t buf_size) {
  if (n < 0 || static_cast<size_t>(n) > buf_size) StdioFortify("fgets");
  return std::fgets(s, n, F(f));
}
size_t BIONIC_SYSV FreadChk(void* buf, size_t size, size_t n, void* f, size_t buf_size) {
  if (size != 0 && n > buf_size / size) StdioFortify("fread");
  return std::fread(buf, size, n, F(f));
}
size_t BIONIC_SYSV FwriteChk(const void* buf, size_t size, size_t n, void* f, size_t buf_size) {
  if (size != 0 && n > buf_size / size) StdioFortify("fwrite");
  return std::fwrite(buf, size, n, F(f));
}
int BIONIC_SYSV Fgetc(void* f) { return std::fgetc(F(f)); }
int BIONIC_SYSV Getchar() { return std::getchar(); }
int BIONIC_SYSV Ungetc(int c, void* f) { return std::ungetc(c, F(f)); }
int BIONIC_SYSV Fflush(void* f) { return std::fflush(f ? F(f) : nullptr); }
int BIONIC_SYSV Feof(void* f) { return std::feof(F(f)); }
int BIONIC_SYSV Ferror(void* f) { return std::ferror(F(f)); }
void BIONIC_SYSV Clearerr(void* f) { std::clearerr(F(f)); }
int BIONIC_SYSV Fileno(void* f) { return _fileno(F(f)); }
int BIONIC_SYSV Fseek(void* f, int64_t off, int whence) { return _fseeki64(F(f), off, whence); }
int64_t BIONIC_SYSV Ftell(void* f) { return _ftelli64(F(f)); }
void BIONIC_SYSV Rewind(void* f) { std::rewind(F(f)); }
int BIONIC_SYSV Setvbuf(void* f, char* buf, int mode, size_t size) {
  // bionic: _IOFBF 0, _IOLBF 1, _IONBF 2; the CRT: 0x0000, 0x0040, 0x0004.
  static const int kModes[] = {_IOFBF, _IOLBF, _IONBF};
  if (mode < 0 || mode > 2) return -1;
  return std::setvbuf(F(f), buf, kModes[mode], size);
}
void BIONIC_SYSV Setbuf(void* f, char* buf) { std::setbuf(F(f), buf); }
FILE* BIONIC_SYSV Tmpfile() { return std::tmpfile(); }
void BIONIC_SYSV Perror(const char* s) {
  if (s != nullptr && *s != 0) {
    std::fprintf(stderr, "%s: %s\n", s, strerror(errno));
  } else {
    std::fprintf(stderr, "%s\n", strerror(errno));
  }
}

// --- scanf -----------------------------------------------------------------

constexpr int kMaxScanArgs = 24;

// A scanf call for the CRT: the format with bionic's LP64 lengths mapped,
// the .so's pointers, and binary128 (%Lf) results read as doubles first.
struct ScanCall {
  std::string fmt;
  void* args[kMaxScanArgs] = {};
  int nargs = 0;
  struct LongDouble {
    void* dst;
    double tmp;
  } long_doubles[kMaxScanArgs];
  int nlong = 0;
};

// A NaN payload sscanf never produces: "not assigned".
constexpr uint64_t kUnassigned = 0x7ff4dead5eed1234ULL;

bool PrepareScan(const char* fmt, SysvVaList* ap, ScanCall* c) {
  std::string& out = c->fmt;
  const char* p = fmt;
  while (*p != 0) {
    if (*p != '%') {
      out += *p++;
      continue;
    }
    out += *p++;
    if (*p == '%') {
      out += *p++;
      continue;
    }
    bool suppress = false;
    if (*p == '*') {
      suppress = true;
      out += *p++;
    }
    while (*p >= '0' && *p <= '9') out += *p++;
    // Lengths: integer 'l', 'j', 'z', 't' are 64-bit in the .so.
    std::string len;
    while (*p != 0 && std::strchr("hlLjztq", *p) != nullptr) len += *p++;
    char conv = *p;
    if (conv == 0) break;
    bool integer = std::strchr("diouxXn", conv) != nullptr;
    if (integer && (len == "l" || len == "j" || len == "z" || len == "t" || len == "q" ||
                    len == "L")) {
      len = "ll";
    }
    bool long_double = !integer && len == "L";
    if (long_double) len = "l";  // read a double, widened afterwards
    out += len;
    if (conv == '[') {
      out += *p++;
      if (*p == '^') out += *p++;
      if (*p == ']') out += *p++;
      while (*p != 0 && *p != ']') out += *p++;
      if (*p == ']') out += *p++;
    } else {
      out += *p++;
    }
    if (!suppress) {
      if (c->nargs == kMaxScanArgs) return false;
      void* arg = reinterpret_cast<void*>(NextGp(ap));
      if (long_double) {
        auto& ld = c->long_doubles[c->nlong++];
        ld.dst = arg;
        std::memcpy(&ld.tmp, &kUnassigned, 8);
        arg = &ld.tmp;
      }
      c->args[c->nargs++] = arg;
    }
  }
  return true;
}

int FinishScan(ScanCall* c, int rc) {
  for (int i = 0; i < c->nlong; ++i) {
    if (std::memcmp(&c->long_doubles[i].tmp, &kUnassigned, 8) != 0) {
      DoubleToBinary128(c->long_doubles[i].tmp, c->long_doubles[i].dst);
    }
  }
  return rc;
}

#define GESHER_SCAN_ARGS(c)                                                                \
  c.args[0], c.args[1], c.args[2], c.args[3], c.args[4], c.args[5], c.args[6], c.args[7],  \
      c.args[8], c.args[9], c.args[10], c.args[11], c.args[12], c.args[13], c.args[14],    \
      c.args[15], c.args[16], c.args[17], c.args[18], c.args[19], c.args[20], c.args[21],  \
      c.args[22], c.args[23]

int VSscanfImpl(const char* str, const char* fmt, SysvVaList* ap) {
  ScanCall c;
  if (!PrepareScan(fmt, ap, &c)) return -1;
  return FinishScan(&c, std::sscanf(str, c.fmt.c_str(), GESHER_SCAN_ARGS(c)));
}

int VFscanfImpl(FILE* f, const char* fmt, SysvVaList* ap) {
  ScanCall c;
  if (!PrepareScan(fmt, ap, &c)) return -1;
  return FinishScan(&c, std::fscanf(f, c.fmt.c_str(), GESHER_SCAN_ARGS(c)));
}

// --- liblog ----------------------------------------------------------------

char PriorityLetter(int prio) {
  static const char kLetters[] = "??VDIWEFS";
  return prio >= 0 && prio <= 8 ? kLetters[prio] : '?';
}

// One WriteFile per line: stderr is unbuffered, and the CRT would write a
// formatted line in pieces that other threads' output (Java's System.out on
// the same file) can land between.
int LogWrite(int prio, const char* tag, const char* text) {
  std::string line(1, PriorityLetter(prio));
  line += '/';
  line += tag ? tag : "";
  line += ": ";
  line += text ? text : "";
  line += '\n';
  std::fflush(stderr);
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  DWORD written = 0;
  if (h == nullptr || h == INVALID_HANDLE_VALUE ||
      !WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
  }
  return 1;
}

}  // namespace

// --- the System V entry points (va_list forms) ------------------------------

extern "C" {

int BIONIC_SYSV gesher_bionic_vsnprintf(char* buf, size_t n, const char* fmt, SysvVaList* ap) {
  std::string s = Format(fmt, ap);
  if (n != 0) {
    size_t c = s.size() < n - 1 ? s.size() : n - 1;
    std::memcpy(buf, s.data(), c);
    buf[c] = 0;
  }
  return static_cast<int>(s.size());
}
int BIONIC_SYSV gesher_bionic_vsprintf(char* buf, const char* fmt, SysvVaList* ap) {
  std::string s = Format(fmt, ap);
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return static_cast<int>(s.size());
}
int BIONIC_SYSV gesher_bionic_vfprintf(void* f, const char* fmt, SysvVaList* ap) {
  std::string s = Format(fmt, ap);
  return std::fwrite(s.data(), 1, s.size(), F(f)) == s.size() ? static_cast<int>(s.size()) : -1;
}
int BIONIC_SYSV gesher_bionic_vprintf(const char* fmt, SysvVaList* ap) {
  return gesher_bionic_vfprintf(stdout, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vdprintf(int fd, const char* fmt, SysvVaList* ap) {
  std::string s = Format(fmt, ap);
  return write(fd, s.data(), static_cast<unsigned>(s.size()));
}
int BIONIC_SYSV gesher_bionic_vasprintf(char** out, const char* fmt, SysvVaList* ap) {
  std::string s = Format(fmt, ap);
  *out = static_cast<char*>(std::malloc(s.size() + 1));
  if (*out == nullptr) return -1;
  std::memcpy(*out, s.c_str(), s.size() + 1);
  return static_cast<int>(s.size());
}
int BIONIC_SYSV gesher_bionic_vsnprintf_chk(char* buf, size_t n, int /*flags*/, size_t slen,
                                           const char* fmt, SysvVaList* ap) {
  if (n > slen) {
    std::fprintf(stderr, "gesher bionic: FORTIFY: vsnprintf: buffer overflow detected\n");
    std::abort();
  }
  return gesher_bionic_vsnprintf(buf, n, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vsprintf_chk(char* buf, int /*flags*/, size_t slen, const char* fmt,
                                          SysvVaList* ap) {
  std::string s = Format(fmt, ap);
  if (s.size() >= slen) {
    std::fprintf(stderr, "gesher bionic: FORTIFY: vsprintf: buffer overflow detected\n");
    std::abort();
  }
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return static_cast<int>(s.size());
}
int BIONIC_SYSV gesher_bionic_vsscanf(const char* str, const char* fmt, SysvVaList* ap) {
  return VSscanfImpl(str, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vfscanf(void* f, const char* fmt, SysvVaList* ap) {
  return VFscanfImpl(F(f), fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vscanf(const char* fmt, SysvVaList* ap) {
  return VFscanfImpl(stdin, fmt, ap);
}
// swprintf: the format as UTF-8 through the same formatter, back to UTF-32.
// -1 when the result (and its NUL) does not fit, as glibc and bionic do.
int BIONIC_SYSV gesher_bionic_vswprintf(uint32_t* buf, size_t n, const uint32_t* fmt,
                                        SysvVaList* ap) {
  std::string s = Format(Utf32ToUtf8(fmt, -1).c_str(), ap);
  std::vector<uint32_t> w = Utf8ToUtf32(s);
  if (n == 0) return -1;
  size_t c = w.size() < n - 1 ? w.size() : n - 1;
  std::memcpy(buf, w.data(), c * sizeof(uint32_t));
  buf[c] = 0;
  return w.size() < n ? static_cast<int>(w.size()) : -1;
}
int BIONIC_SYSV gesher_bionic_android_log_vprint(int prio, const char* tag, const char* fmt,
                                                SysvVaList* ap) {
  return LogWrite(prio, tag, Format(fmt, ap).c_str());
}
int BIONIC_SYSV gesher_bionic_android_log_buf_vprint(int /*buf*/, int prio, const char* tag,
                                                    const char* fmt, SysvVaList* ap) {
  return LogWrite(prio, tag, Format(fmt, ap).c_str());
}
void BIONIC_SYSV gesher_bionic_android_log_vassert(const char* cond, const char* tag,
                                                  const char* fmt, SysvVaList* ap) {
  if (fmt != nullptr) {
    LogWrite(7, tag, Format(fmt, ap).c_str());
  } else {
    LogWrite(7, tag, cond ? cond : "assertion failed");
  }
  std::abort();
}
void BIONIC_SYSV gesher_bionic_vsyslog(int prio, const char* fmt, SysvVaList* ap) {
  LogWrite(prio <= 3 ? 6 : prio <= 4 ? 5 : prio <= 6 ? 4 : 3, "syslog", Format(fmt, ap).c_str());
}
int BIONIC_SYSV gesher_bionic_android_log_write(int prio, const char* tag, const char* text) {
  return LogWrite(prio, tag, text);
}
int BIONIC_SYSV gesher_bionic_android_log_buf_write(int /*buf*/, int prio, const char* tag,
                                                   const char* text) {
  return LogWrite(prio, tag, text);
}
int BIONIC_SYSV gesher_bionic_android_log_is_loggable(int, const char*, int) { return 1; }

// The variadic entry points, defined in assembly below.
void gesher_bionic_printf();
void gesher_bionic_fprintf();
void gesher_bionic_sprintf();
void gesher_bionic_snprintf();
void gesher_bionic_dprintf();
void gesher_bionic_asprintf();
void gesher_bionic_sscanf();
void gesher_bionic_fscanf();
void gesher_bionic_scanf();
void gesher_bionic_swprintf();
void gesher_bionic_syslog();
void gesher_bionic_android_log_print();
void gesher_bionic_android_log_assert();
void gesher_bionic_android_log_buf_print();
void gesher_bionic_sprintf_chk();
void gesher_bionic_snprintf_chk();

}  // extern "C"

// The trampolines (GESHER_VA_TRAMPOLINE, shim_internal.h).
asm(".text\n"
    GESHER_VA_TRAMPOLINE("gesher_bionic_printf", "gesher_bionic_vprintf", "8", "%rsi")
    GESHER_VA_TRAMPOLINE("gesher_bionic_fprintf", "gesher_bionic_vfprintf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_sprintf", "gesher_bionic_vsprintf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_snprintf", "gesher_bionic_vsnprintf", "24", "%rcx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_dprintf", "gesher_bionic_vdprintf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_asprintf", "gesher_bionic_vasprintf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_sscanf", "gesher_bionic_vsscanf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_fscanf", "gesher_bionic_vfscanf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_scanf", "gesher_bionic_vscanf", "8", "%rsi")
    GESHER_VA_TRAMPOLINE("gesher_bionic_swprintf", "gesher_bionic_vswprintf", "24", "%rcx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_syslog", "gesher_bionic_vsyslog", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_android_log_print",
                         "gesher_bionic_android_log_vprint", "24", "%rcx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_android_log_assert",
                         "gesher_bionic_android_log_vassert", "24", "%rcx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_android_log_buf_print",
                         "gesher_bionic_android_log_buf_vprint", "32", "%r8")
    GESHER_VA_TRAMPOLINE("gesher_bionic_sprintf_chk", "gesher_bionic_vsprintf_chk", "32", "%r8")
    GESHER_VA_TRAMPOLINE("gesher_bionic_snprintf_chk", "gesher_bionic_vsnprintf_chk", "40",
                         "%r9"));

void RegisterStdio(SymbolTable& t) {
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)
  // Formatted output, variadic and va_list forms.
  E("printf", gesher_bionic_printf);
  E("fprintf", gesher_bionic_fprintf);
  E("sprintf", gesher_bionic_sprintf);
  E("snprintf", gesher_bionic_snprintf);
  E("dprintf", gesher_bionic_dprintf);
  E("asprintf", gesher_bionic_asprintf);
  E("vprintf", gesher_bionic_vprintf);
  E("vfprintf", gesher_bionic_vfprintf);
  E("vsprintf", gesher_bionic_vsprintf);
  E("vsnprintf", gesher_bionic_vsnprintf);
  E("vdprintf", gesher_bionic_vdprintf);
  E("vasprintf", gesher_bionic_vasprintf);
  E("__sprintf_chk", gesher_bionic_sprintf_chk);
  E("__snprintf_chk", gesher_bionic_snprintf_chk);
  E("__vsprintf_chk", gesher_bionic_vsprintf_chk);
  E("__vsnprintf_chk", gesher_bionic_vsnprintf_chk);
  E("sscanf", gesher_bionic_sscanf);
  E("vsscanf", gesher_bionic_vsscanf);
  E("fscanf", gesher_bionic_fscanf);
  E("vfscanf", gesher_bionic_vfscanf);
  E("scanf", gesher_bionic_scanf);
  E("vscanf", gesher_bionic_vscanf);
  E("swprintf", gesher_bionic_swprintf);
  E("vswprintf", gesher_bionic_vswprintf);
  E("syslog", gesher_bionic_syslog);
  E("vsyslog", gesher_bionic_vsyslog);
  // FILE.
  E("fopen", Fopen);
  E("fopen64", Fopen);
  E("fdopen", Fdopen);
  E("freopen", Freopen);
  E("freopen64", Freopen);
  E("fgetpos", Fgetpos);
  E("fgetpos64", Fgetpos);
  E("fsetpos", Fsetpos);
  E("fsetpos64", Fsetpos);
  E("getline", Getline);
  E("getdelim", Getdelim);
  E("fgetwc", Fgetwc);
  E("getwc", Fgetwc);
  E("getwchar", Getwchar);
  E("ungetwc", Ungetwc);
  E("fputwc", Fputwc);
  E("putwc", Fputwc);
  E("putwchar", Putwchar);
  E("fputws", Fputws);
  E("fwide", Fwide);
  E("fclose", Fclose);
  E("fread", Fread);
  E("fwrite", Fwrite);
  E("fputs", Fputs);
  E("fputc", Fputc);
  E("putc", Fputc);
  E("putchar", Putchar);
  E("puts", Puts);
  E("fgets", Fgets);
  E("__fgets_chk", FgetsChk);
  E("__fread_chk", FreadChk);
  E("__fwrite_chk", FwriteChk);
  E("fgetc", Fgetc);
  E("getc", Fgetc);
  E("getchar", Getchar);
  E("ungetc", Ungetc);
  E("fflush", Fflush);
  E("feof", Feof);
  E("ferror", Ferror);
  E("clearerr", Clearerr);
  E("fileno", Fileno);
  E("fseek", Fseek);
  E("fseeko", Fseek);
  E("fseeko64", Fseek);
  E("ftell", Ftell);
  E("ftello", Ftell);
  E("ftello64", Ftell);
  E("rewind", Rewind);
  E("setvbuf", Setvbuf);
  E("setbuf", Setbuf);
  E("tmpfile", Tmpfile);
  E("perror", Perror);
  E("stdin", g_stdin);
  E("stdout", g_stdout);
  E("stderr", g_stderr);
  E("__sF", g_sF);
  // liblog.
  E("__android_log_write", gesher_bionic_android_log_write);
  E("__android_log_buf_write", gesher_bionic_android_log_buf_write);
  E("__android_log_print", gesher_bionic_android_log_print);
  E("__android_log_buf_print", gesher_bionic_android_log_buf_print);
  E("__android_log_vprint", gesher_bionic_android_log_vprint);
  E("__android_log_assert", gesher_bionic_android_log_assert);
  E("__android_log_is_loggable", gesher_bionic_android_log_is_loggable);
#undef E
}

}  // namespace gesher::bionic
