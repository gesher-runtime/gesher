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
// extensions (`%m`, `%p` as 0x...). The scanf family builds a Windows va_list
// of destination pointers and maps the format's lengths the same way.

#include "shim_internal.h"
#include "stdio_windows_lock.h"
#include <gesher/bionic_shim.h>
#include <gesher/logging.h>
#include <gesher/stdio.h>

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

extern "C" int32_t __android_log_get_minimum_priority();
extern "C" int32_t __android_log_set_minimum_priority(int32_t priority);
extern "C" void __android_log_set_default_tag(const char* tag);
extern "C" int __android_log_is_loggable_len(int priority, const char* tag, size_t length, int fallback);
extern "C" const char* gesher_android_default_log_tag();

namespace gesher::bionic {

namespace {

// bionic's x86-64 long double is IEEE binary128, passed like an SSE value of
// 16 bytes. Narrowed to double for the CRT (which has no binary128).
double NextFp128(SysvVaList* ap) {
  uint64_t lo, hi;
  const uint8_t* source = NextFpArea(ap, 16);
  std::memcpy(&lo, source, 8);
  std::memcpy(&hi, source + 8, 8);
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
  for (int i = 0; (max_units < 0 || i < max_units) && s[i] != 0; ++i) {
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

constexpr int kEilseq = 84;  // Linux EILSEQ
constexpr int kEoverflow = 75;  // Linux EOVERFLOW
size_t DecodeWide(uint32_t* wc, const char* input, size_t size, void* state);
size_t EncodeWide(char* output, uint32_t wc);
size_t DecodeWideBuffer(uint32_t* output, const char** input, size_t count);

using WideText = std::vector<uint32_t>;

// Stream printf commits each completed field before processing the next one.
// The buffer variant supplies an encoded-byte sink, matching Bionic vswprintf's
// intermediate multibyte stream. No complete UTF-32 result is retained.
struct WideSink {
  void* context;
  bool (*write)(void*, uint32_t);
  size_t count = 0;

  bool Put(uint32_t wc) {
    if (count == INT_MAX) return errno = kENOMEM, false;
    if (!write(context, wc)) return false;
    ++count;
    return true;
  }

  bool Append(const WideText& text) {
    for (uint32_t wc : text) {
      if (!Put(wc)) return false;
    }
    return true;
  }
};

// A narrow %s argument to wide printf is a multibyte string. Precision
// bounds the number of converted wide characters, not the number of bytes.
bool DecodeWideString(const char* input, int precision, WideText* text) {
  std::array<uint8_t, 8> state{};
  bool partial = false;
  while ((precision < 0 || text->size() < static_cast<size_t>(precision)) && *input != 0) {
    uint32_t wc;
    size_t result = DecodeWide(&wc, input++, 1, state.data());
    if (result == static_cast<size_t>(-1)) return false;
    partial = result == static_cast<size_t>(-2);
    if (!partial) text->push_back(wc);
  }
  if (partial) errno = kEilseq;
  return !partial;
}

void AppendWideField(WideText* out, const WideText& value, int width, bool left) {
  size_t padding = width > 0 && static_cast<size_t>(width) > value.size() ?
      static_cast<size_t>(width) - value.size() : 0;
  if (!left) out->insert(out->end(), padding, ' ');
  out->insert(out->end(), value.begin(), value.end());
  if (left) out->insert(out->end(), padding, ' ');
}

bool WriteWideField(WideSink* sink, const uint32_t* value, size_t size, int width, bool left) {
  size_t padding = width > 0 && static_cast<size_t>(width) > size ?
      static_cast<size_t>(width) - size : 0;
  if (!left) for (size_t i = 0; i < padding; ++i) if (!sink->Put(' ')) return false;
  for (size_t i = 0; i < size; ++i) if (!sink->Put(value[i])) return false;
  if (left) for (size_t i = 0; i < padding; ++i) if (!sink->Put(' ')) return false;
  return true;
}

// Numeric conversions are rendered by the same CRT formatter in both modes.
// Bionic's supported C locales produce ASCII digits, signs and punctuation.
template <class... Args>
void AppendF(WideText* out, const char* spec, Args... args) {
  std::string field;
  AppendF(&field, spec, args...);
  out->insert(out->end(), field.begin(), field.end());
}

enum class Len { kNone, kHH, kH, kL, kLL, kBigL, kJ, kZ, kT };

template <class Char>
using FormatText = std::conditional_t<std::is_same_v<Char, char>, std::string, WideText>;

template <class Char>
bool SkipArgumentNumber(const Char*& cursor) {
  const Char* end = cursor;
  while (*end >= '0' && *end <= '9') ++end;
  if (end == cursor || *end != '$') return false;
  cursor = end + 1;
  return true;
}

template <class Char>
FormatText<Char> Format(const Char* fmt, SysvVaList* ap, bool* valid = nullptr,
                        WideSink* sink = nullptr) {
  constexpr bool wide = !std::is_same_v<Char, char>;
  FormatText<Char> out;
  bool local_valid;
  if (!valid) valid = &local_valid;
  *valid = true;
  const Char* p = fmt;
  while (*p != 0) {
    if constexpr (wide) {
      if (sink != nullptr) {
        if (!sink->Append(out)) { *valid = false; return {}; }
        out.clear();
      }
    }
    if (*p != '%') {
      const Char* begin = p;
      if constexpr (wide) {
        if (sink != nullptr) {
          do {
            if (!sink->Put(*p++)) { *valid = false; return {}; }
          } while (*p != 0 && *p != '%');
          continue;
        }
        do { ++p; } while (*p != 0 && *p != '%');
      } else {
        const char* next = std::strchr(p, '%');
        p = next ? next : p + std::strlen(p);
      }
      out.insert(out.end(), begin, p);
      continue;
    }
    const Char* start = p++;
    if (*p == '%') {
      out.push_back('%');
      ++p;
      continue;
    }
    // Positional values remain outside this bridge's scope, but their
    // selectors must never suppress Android's FORTIFY rule for printf %n.
    bool positional = SkipArgumentNumber(p);
    std::string flags;
    while (*p > 0 && *p <= 0x7f && std::strchr("-+ #0'", *p) != nullptr) {
      if (*p != '\'') flags += *p;  // the CRT has no grouping flag
      ++p;
    }
    int width = -1;
    if (*p == '*') {
      ++p;
      positional = SkipArgumentNumber(p) || positional;
      if (!positional) {
        width = static_cast<int>(NextGp(ap));
        if (width < 0) {
          if (width == INT_MIN) {
            errno = kENOMEM;
            *valid = false;
            return {};
          }
          flags += '-';
          width = -width;
        }
      }
    } else {
      while (*p >= '0' && *p <= '9') {
        int digit = *p++ - '0';
        if (width > (INT_MAX - digit) / 10) {
          errno = kENOMEM; *valid = false; return {};
        }
        width = (width < 0 ? 0 : width * 10) + digit;
      }
    }
    int prec = -1;
    if (*p == '.') {
      ++p;
      if (*p == '*') {
        ++p;
        positional = SkipArgumentNumber(p) || positional;
        if (!positional) prec = static_cast<int>(NextGp(ap));
      } else {
        prec = 0;
        while (*p >= '0' && *p <= '9') {
          int digit = *p++ - '0';
          if (prec > (INT_MAX - digit) / 10) {
            errno = kENOMEM; *valid = false; return {};
          }
          prec = prec * 10 + digit;
        }
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
      case 'w': {
        // Fixed-width conversions are not implemented here. Recognize the
        // length syntax when followed by %n so FORTIFY still terminates.
        const Char* end = p + 1;
        if (*end == 'f') ++end;
        while (*end >= '0' && *end <= '9') ++end;
        if (*end == 'n') p = end;
        break;
      }
      default: break;
    }
    if (*p == 0) {
      if constexpr (!wide) out.insert(out.end(), start, p);
      break;
    }
    Char conversion = *p++;
    if (positional && conversion != 'n') {
      out.insert(out.end(), start, p);
      continue;
    }
    if (conversion < 0 || conversion > 0x7f) {
      if constexpr (wide) out.push_back(conversion);
      else out.insert(out.end(), start, p);
      continue;
    }
    char conv = static_cast<char>(conversion);
    if constexpr (wide) {
      // Android's uppercase aliases force the corresponding long conversion.
      if (conv == 'C' || conv == 'S' || conv == 'D' || conv == 'O' || conv == 'U') {
        len = Len::kL;
        conv = static_cast<char>(conv - 'A' + 'a');
      }
    }

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
        if constexpr (wide) {
          uint32_t wc = static_cast<uint32_t>(raw);
          if (len != Len::kL) {
            unsigned char byte = static_cast<unsigned char>(raw);
            // Bionic btowc yields WEOF for a non-ASCII byte. Let the output
            // encoder report EILSEQ after any field padding has been written.
            wc = byte < 0x80 ? byte : 0xffffffffu;
          }
          if (sink != nullptr) {
            if (!WriteWideField(sink, &wc, 1, width, flags.find('-') != std::string::npos)) {
              *valid = false; return {};
            }
          } else AppendWideField(&out, {wc}, width, flags.find('-') != std::string::npos);
        } else if (len == Len::kL) {
          uint32_t wc[2] = {static_cast<uint32_t>(raw), 0};
          AppendF(&out, (spec_no_prec + "s").c_str(), Utf32ToUtf8(wc, -1).c_str());
        } else {
          AppendF(&out, (spec_no_prec + "c").c_str(), static_cast<int>(raw));
        }
        break;
      }
      case 's': {
        auto ptr = reinterpret_cast<const void*>(NextGp(ap));
        if constexpr (wide) {
          WideText text;
          if (ptr == nullptr) {
            DecodeWideString("(null)", prec, &text);
          } else if (len == Len::kL) {
            auto ws = static_cast<const uint32_t*>(ptr);
            if (sink != nullptr) {
              size_t length = 0;
              while ((prec < 0 || length < static_cast<size_t>(prec)) && ws[length] != 0) ++length;
              if (!WriteWideField(sink, ws, length, width, flags.find('-') != std::string::npos)) {
                *valid = false; return {};
              }
              break;
            }
            while ((prec < 0 || text.size() < static_cast<size_t>(prec)) && *ws != 0)
              text.push_back(*ws++);
          } else if (!DecodeWideString(static_cast<const char*>(ptr), prec, &text)) {
            *valid = false;
            return {};
          }
          if (sink != nullptr) {
            if (!WriteWideField(sink, text.data(), text.size(), width, flags.find('-') != std::string::npos)) {
              *valid = false; return {};
            }
          } else AppendWideField(&out, text, width, flags.find('-') != std::string::npos);
        } else if (ptr == nullptr) {
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
      case 'n':
        // Bionic forbids every printf %n, independently of destination and
        // length modifier. Scanf's separate conversion remains supported.
        std::fputs("FORTIFY: %n not allowed on Android\n", stderr);
        std::abort();
      case 'm':  // bionic/glibc: strerror(errno), no argument
        AppendF(&out, (spec + "s").c_str(), strerror(errno));
        break;
      default:
        if constexpr (wide) out.push_back(conversion);
        else out.insert(out.end(), start, p);
        break;
    }
  }
  if constexpr (wide) {
    if (sink != nullptr) {
      if (!sink->Append(out)) { *valid = false; return {}; }
      out.clear();
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

// Bionic conversion metadata belongs to the same shared stream lifetime as
// descriptor-backed I/O, orientation and locking. A host fclose/freopen must
// retire it too; a second FILE-address registry could outlive a recycled token.
constexpr uint32_t kWeof = 0xffffffffu;
using StreamState = gesher::stdio::BionicState;
// Locked entry points already hold the shared owner/CRT lock; unlocked entry
// points must rely on their caller's synchronization instead of acquiring it.
int Orient(FILE* stream, int mode) { return gesher::stdio::Orient(stream, mode, true); }

using StreamLock = gesher::stdio::StreamGuard;

void BIONIC_SYSV Flockfile(void* f) { gesher::stdio::Lock(F(f)); }
void BIONIC_SYSV Funlockfile(void* f) { gesher::stdio::Unlock(F(f)); }
int BIONIC_SYSV Ftrylockfile(void* f) { return gesher::stdio::TryLock(F(f)); }

}  // namespace

int CloseBionicStream(FILE* stream, int* retired_out) {
  // The POSIX owner removes all stream metadata before native close releases
  // FILE. Preserve the retirement outcome for pclose's child association.
  return gesher_fclose_state(stream, retired_out);
}

int CloseBionicStream(FILE* stream) { return CloseBionicStream(stream, nullptr); }

namespace {

// Descriptor-backed streams cover regular files, pipes and virtual devices
// through the same shared open-file description and buffering path.
size_t ReadStream(void* buf, size_t size, size_t n, FILE* f) {
  return gesher::stdio::Read(buf, size, n, f, true);
}

// fopen through gesher-base's open (Linux O_* flags, delete access shared, so
// the file can be unlinked or renamed while open, as on Linux), then fdopen.
FILE* BIONIC_SYSV Fopen(const char* path, const char* mode) {
  return std::fopen(path, mode);
}
FILE* BIONIC_SYSV Fdopen(int fd, const char* mode) { return fdopen(fd, mode); }
FILE* BIONIC_SYSV Freopen(const char* path, const char* mode, void* f) {
  // The POSIX owner applies the same Android mode parser as fopen/fdopen,
  // including truthful unsupported mode-only reopen and metadata retirement.
  return gesher_freopen_state(path, mode, F(f), nullptr);
}
int BIONIC_SYSV Fgetpos(void* f, int64_t* pos) {
  int64_t p = gesher::stdio::Tell(F(f));
  if (p < 0) return -1;
  *pos = p;
  return 0;
}
int BIONIC_SYSV Fsetpos(void* f, const int64_t* pos) { return static_cast<int>(gesher::stdio::Seek(F(f), *pos, SEEK_SET)); }

// getline/getdelim: *line grows with the CRT's realloc, which the .so's
// free (the shim's) accepts.
int64_t BIONIC_SYSV Getdelim(char** line, size_t* n, int delim, void* f) {
  if (line == nullptr || n == nullptr) return errno = kEINVAL, -1;
  FILE* fp = F(f);
  StreamLock lock(fp);
  if (!lock) return -1;
  if (Orient(fp, -1) == 0) return -1;
  size_t len = 0;
  int c;
  while ((c = gesher::stdio::Get(fp, true)) != EOF) {
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

// Use the registered Bionic converters rather than Windows wchar_t or the
// Windows process locale. Android wchar_t/wint_t occupy 32 bits on both ABIs.
size_t DecodeWide(uint32_t* wc, const char* input, size_t size, void* state) {
  using Decode = size_t(BIONIC_SYSV*)(uint32_t*, const char*, size_t, void*);
  static const auto decode = reinterpret_cast<Decode>(LookupBionicSymbol("mbrtowc"));
  return decode(wc, input, size, state);
}

size_t EncodeWide(char* output, uint32_t wc) {
  using Encode = size_t(BIONIC_SYSV*)(char*, uint32_t, void*);
  static const auto encode = reinterpret_cast<Encode>(LookupBionicSymbol("wcrtomb"));
  std::array<uint8_t, 8> state{};
  return encode(output, wc, state.data());
}

size_t DecodeWideBuffer(uint32_t* output, const char** input, size_t count) {
  using Decode = size_t(BIONIC_SYSV*)(uint32_t*, const char**, size_t, void*);
  static const auto decode = reinterpret_cast<Decode>(LookupBionicSymbol("mbsrtowcs"));
  std::array<uint8_t, 8> state{};
  return decode(output, input, count, state.data());
}

uint32_t ReadWide(FILE* stream, StreamState& state) {
  if (state.pushback != kWeof) {
    uint32_t wc = state.pushback;
    state.pushback = kWeof;
    return wc;
  }
  while (true) {
    int byte = gesher::stdio::Get(stream, true);
    if (byte == EOF) return kWeof;
    char input = static_cast<char>(byte);
    uint32_t wc = 0;
    size_t converted = DecodeWide(&wc, &input, 1, state.input_state.data());
    if (converted == static_cast<size_t>(-2)) continue;
    if (converted == static_cast<size_t>(-1)) {
      state.conversion_error = true;
      return kWeof;
    }
    return wc;
  }
}

bool WriteWide(FILE* stream, StreamState& state, uint32_t wc) {
  state.pushback = kWeof;
  char bytes[4];
  size_t size = EncodeWide(bytes, wc);
  if (size == static_cast<size_t>(-1)) {
    state.conversion_error = true;
    return false;
  }
  return gesher::stdio::Write(bytes, 1, size, stream, true) == size;
}

uint32_t BIONIC_SYSV Fgetwc(void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return kWeof;
  auto* state = lock.Metadata();
  if (!state || Orient(stream, 1) == 0) return kWeof;
  return ReadWide(stream, *state);
}

uint32_t* BIONIC_SYSV Fgetws(uint32_t* out, int n, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return nullptr;
  auto* state = lock.Metadata();
  if (!state || Orient(stream, 1) == 0) return nullptr;
  if (n <= 0) return errno = kEINVAL, nullptr;
  int count = 0;
  while (count < n - 1) {
    uint32_t wc = ReadWide(stream, *state);
    if (wc == kWeof) {
      if (count == 0 || (errno == kEilseq && (state->conversion_error || std::ferror(stream))))
        return nullptr;
      break;
    }
    out[count++] = wc;
    if (wc == '\n') break;
  }
  out[count] = 0;
  return out;
}

uint32_t BIONIC_SYSV Ungetwc(uint32_t c, void* f) {
  if (c == kWeof) return kWeof;
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return kWeof;
  auto* state = lock.Metadata();
  if (!state || Orient(stream, 1) == 0) return kWeof;
  if (state->pushback != kWeof) return kWeof;
  state->pushback = c;
  state->conversion_error = false;
  std::clearerr(stream);
  return c;
}

uint32_t BIONIC_SYSV Fputwc(uint32_t c, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return kWeof;
  auto* state = lock.Metadata();
  if (!state || Orient(stream, 1) == 0) return kWeof;
  return WriteWide(stream, *state, c) ? c : kWeof;
}

int BIONIC_SYSV Fputws(const uint32_t* ws, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return -1;
  auto* state = lock.Metadata();
  if (!state || Orient(stream, 1) == 0) return -1;
  for (; *ws != 0; ++ws) {
    if (!WriteWide(stream, *state, *ws)) return -1;
  }
  return 0;
}

uint32_t BIONIC_SYSV Getwchar() { return Fgetwc(stdin); }
uint32_t BIONIC_SYSV Putwchar(uint32_t c) { return Fputwc(c, stdout); }
int BIONIC_SYSV Fwide(void* f, int mode) {
  return gesher::stdio::Orient(F(f), mode);
}
int BIONIC_SYSV Fclose(void* f) { return CloseBionicStream(F(f)); }

int BIONIC_SYSV GetcUnlocked(void* f) {
  FILE* stream = F(f);
  if (Orient(stream, -1) == 0) return EOF;
  return gesher::stdio::Get(stream, true);
}
int BIONIC_SYSV PutcUnlocked(int c, void* f) {
  FILE* stream = F(f);
  if (Orient(stream, -1) == 0) return EOF;
  return gesher::stdio::Put(c, stream, true);
}
int BIONIC_SYSV GetcharUnlocked() { return GetcUnlocked(stdin); }
int BIONIC_SYSV PutcharUnlocked(int c) { return PutcUnlocked(c, stdout); }
size_t BIONIC_SYSV FreadUnlocked(void* buf, size_t size, size_t n, void* f) {
  FILE* stream = F(f);
  if (size != 0 && n != 0 && Orient(stream, -1) == 0) return 0;
  return ReadStream(buf, size, n, stream);
}
size_t BIONIC_SYSV FwriteUnlocked(const void* buf, size_t size, size_t n, void* f) {
  FILE* stream = F(f);
  if (size != 0 && n != 0 && Orient(stream, -1) == 0) return 0;
  return gesher::stdio::Write(buf, size, n, stream, true);
}
int BIONIC_SYSV FputsUnlocked(const char* s, void* f) {
  FILE* stream = F(f);
  if (Orient(stream, -1) == 0) return EOF;
  size_t n = std::strlen(s);
  return gesher::stdio::Write(s, 1, n, stream, true) == n ? 0 : EOF;
}
char* BIONIC_SYSV FgetsUnlocked(char* s, int n, void* f) {
  if (n <= 0) return errno = kEINVAL, nullptr;
  FILE* stream = F(f);
  if (Orient(stream, -1) == 0) return nullptr;
  int count = 0;
  while (count < n - 1) {
    int c = gesher::stdio::Get(stream, true);
    if (c == EOF) {
      if (count == 0) return nullptr;
      break;
    }
    s[count++] = static_cast<char>(c);
    if (c == '\n') break;
  }
  s[count] = 0;
  return s;
}

size_t BIONIC_SYSV Fread(void* buf, size_t size, size_t n, void* f) {
  if (size == 0 || n == 0) return 0;
  StreamLock lock(F(f));
  if (!lock) return 0;
  return FreadUnlocked(buf, size, n, f);
}
size_t BIONIC_SYSV Fwrite(const void* buf, size_t size, size_t n, void* f) {
  if (size == 0 || n == 0) return 0;
  StreamLock lock(F(f));
  if (!lock) return 0;
  return FwriteUnlocked(buf, size, n, f);
}
int BIONIC_SYSV Fputs(const char* s, void* f) {
  StreamLock lock(F(f));
  if (!lock) return EOF;
  return FputsUnlocked(s, f);
}
int BIONIC_SYSV Fputc(int c, void* f) {
  StreamLock lock(F(f));
  if (!lock) return EOF;
  return PutcUnlocked(c, f);
}
int BIONIC_SYSV Putchar(int c) { return Fputc(c, stdout); }
int BIONIC_SYSV Puts(const char* s) {
  StreamLock lock(stdout);
  if (!lock) return EOF;
  if (Orient(stdout, -1) == 0) return EOF;
  return std::puts(s);
}
char* BIONIC_SYSV Fgets(char* s, int n, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return nullptr;
  if (n > 0 && Orient(stream, -1) == 0) return nullptr;
  return std::fgets(s, n, stream);
}
[[noreturn]] void StdioFortify(const char* what) {
  std::fprintf(stderr, "gesher bionic: FORTIFY: %s: prevented buffer overflow\n", what);
  std::abort();
}
char* BIONIC_SYSV FgetsChk(char* s, int n, void* f, size_t buf_size) {
  if (n < 0 || static_cast<size_t>(n) > buf_size) StdioFortify("fgets");
  return Fgets(s, n, f);
}
size_t BIONIC_SYSV FreadChk(void* buf, size_t size, size_t n, void* f, size_t buf_size) {
  if (size != 0 && n > buf_size / size) StdioFortify("fread");
  return Fread(buf, size, n, f);
}
size_t BIONIC_SYSV FwriteChk(const void* buf, size_t size, size_t n, void* f, size_t buf_size) {
  if (size != 0 && n > buf_size / size) StdioFortify("fwrite");
  return Fwrite(buf, size, n, f);
}
int BIONIC_SYSV Fgetc(void* f) {
  StreamLock lock(F(f));
  if (!lock) return EOF;
  return GetcUnlocked(f);
}
int BIONIC_SYSV Getchar() { return Fgetc(stdin); }
int BIONIC_SYSV Ungetc(int c, void* f) {
  if (c == EOF) return EOF;
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return EOF;
  int result = std::ungetc(c, stream);
  if (result != EOF) Orient(stream, -1);
  return result;
}
int BIONIC_SYSV Fflush(void* f) { return std::fflush(f ? F(f) : nullptr); }
int BIONIC_SYSV Feof(void* f) { return std::feof(F(f)); }
int BIONIC_SYSV FerrorUnlocked(void* f) {
  FILE* stream = F(f);
  return gesher::stdio::Error(stream, true);
}
int BIONIC_SYSV Ferror(void* f) {
  return gesher::stdio::Error(F(f));
}
void BIONIC_SYSV ClearerrUnlocked(void* f) {
  FILE* stream = F(f);
  gesher::stdio::Clear(stream, true);
}
void BIONIC_SYSV Clearerr(void* f) {
  gesher::stdio::Clear(F(f));
}
int BIONIC_SYSV Fileno(void* f) { return _fileno(F(f)); }
int BIONIC_SYSV Fseek(void* f, int64_t off, int whence) { return static_cast<int>(gesher::stdio::Seek(F(f), off, whence)); }
int64_t BIONIC_SYSV Ftell(void* f) { return gesher::stdio::Tell(F(f)); }
int BIONIC_SYSV Getw(void* f) {
  int word;
  return Fread(&word, sizeof(word), 1, f) == 1 ? word : EOF;
}
int BIONIC_SYSV Putw(int word, void* f) {
  return Fwrite(&word, sizeof(word), 1, f) == 1 ? 0 : EOF;
}
void BIONIC_SYSV Rewind(void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return;
  gesher::stdio::Seek(stream, 0, SEEK_SET);
  // Like Android's rewind, clear stream errors even if seeking failed.
  ClearerrUnlocked(f);
}
int BIONIC_SYSV Setvbuf(void* f, char* buf, int mode, size_t size) {
  // bionic: _IOFBF 0, _IOLBF 1, _IONBF 2; the CRT: 0x0000, 0x0040, 0x0004.
  static const int kModes[] = {_IOFBF, _IOLBF, _IONBF};
  if (mode < 0 || mode > 2) return -1;
  return std::setvbuf(F(f), buf, kModes[mode], size);
}
void BIONIC_SYSV Setbuf(void* f, char* buf) { std::setvbuf(F(f), buf, buf ? _IOFBF : _IONBF, 1024); }
FILE* BIONIC_SYSV Tmpfile() { return std::tmpfile(); }
void BIONIC_SYSV Perror(const char* s) {
  StreamLock lock(stderr);
  if (!lock) return;
  if (Orient(stderr, -1) == 0) return;
  if (s != nullptr && *s != 0) {
    std::fprintf(stderr, "%s: %s\n", s, strerror(errno));
  } else {
    std::fprintf(stderr, "%s\n", strerror(errno));
  }
}

// --- scanf -----------------------------------------------------------------

// On the supported Windows targets va_list walks contiguous 8-byte slots
// for pointer arguments. scanf destinations are all pointers.
static_assert(std::is_same_v<va_list, char*> && sizeof(void*) == 8);

// A NaN payload sscanf never produces: "not assigned".
constexpr uint64_t kUnassigned = 0x7ff4dead5eed1234ULL;

// A scanf call for the CRT: the format with bionic's LP64 lengths mapped,
// the .so's pointers, and binary128 (%Lf) results read as doubles first.
struct ScanCall {
  std::string fmt;
  std::array<void*, 16> inline_args;
  std::vector<void*> overflow_args;
  size_t nargs = 0;
  struct LongDouble {
    size_t index;
    void* dst;
    double tmp;
  };
  std::vector<LongDouble> long_doubles;

  void AddArgument(void* arg, bool long_double) {
    if (long_double) long_doubles.push_back({nargs, arg, 0});
    if (nargs < inline_args.size()) {
      inline_args[nargs] = arg;
    } else {
      if (overflow_args.empty()) {
        overflow_args.reserve(2 * inline_args.size());
        overflow_args.assign(inline_args.begin(), inline_args.end());
      }
      overflow_args.push_back(arg);
    }
    ++nargs;
  }

  va_list CrtArguments() {
    void** args = overflow_args.empty() ? inline_args.data() : overflow_args.data();
    // Take addresses only after parsing, when vector storage is stable.
    for (auto& ld : long_doubles) {
      std::memcpy(&ld.tmp, &kUnassigned, sizeof(ld.tmp));
      args[ld.index] = &ld.tmp;
    }
    return reinterpret_cast<va_list>(args);
  }
};

void PrepareScan(const char* fmt, SysvVaList* ap, ScanCall* c) {
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
      c->AddArgument(reinterpret_cast<void*>(NextGp(ap)), long_double);
    }
  }
}

int FinishScan(ScanCall* c, int rc) {
  for (const auto& ld : c->long_doubles) {
    if (std::memcmp(&ld.tmp, &kUnassigned, sizeof(ld.tmp)) != 0) {
      DoubleToBinary128(ld.tmp, ld.dst);
    }
  }
  return rc;
}

int VSscanfImpl(const char* str, const char* fmt, SysvVaList* ap) {
  ScanCall c;
  PrepareScan(fmt, ap, &c);
  return FinishScan(&c, std::vsscanf(str, c.fmt.c_str(), c.CrtArguments()));
}

int VFscanfImpl(FILE* f, const char* fmt, SysvVaList* ap) {
  ScanCall c;
  PrepareScan(fmt, ap, &c);
  return FinishScan(&c, std::vfscanf(f, c.fmt.c_str(), c.CrtArguments()));
}

// --- liblog ----------------------------------------------------------------

char PriorityLetter(int prio) {
  static const char kLetters[] = "??VDIWEFS";
  return prio >= 0 && prio <= 8 ? kLetters[prio] : '?';
}

// Capture the inherited diagnostic destination before an app can redirect fd 2.
// VLC forwards its stderr pipe to liblog; writing liblog back into that pipe
// creates feedback and can block both its reader and the app's UI thread.
// This duplicate lives until process exit, independently of later dup2/close.
const HANDLE logOutput = [] {
  HANDLE original = GetStdHandle(STD_ERROR_HANDLE);
  HANDLE saved = INVALID_HANDLE_VALUE;
  if (original && original != INVALID_HANDLE_VALUE) {
    DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &saved,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
  }
  return saved;
}();

// One WriteFile per line avoids interleaving the pieces of a formatted line.
int LogWrite(int prio, const char* tag, const char* text, int buffer = 0, bool filter = true) {
  if (!tag) tag = ::gesher_android_default_log_tag();
  if (filter && !::__android_log_is_loggable_len(prio, tag, tag ? std::strlen(tag) : 0, 2)) return -1;
  logging::Record(buffer, prio, tag, text);
  std::string line(1, PriorityLetter(prio));
  line += '/';
  line += tag ? tag : "";
  line += ": ";
  line += text ? text : "";
  // A message's own final newline ends its last line, as logcat prints it
  // (glog-style native loggers end every message with one): no blank line.
  if (line.back() == '\n') line.pop_back();
  line += '\n';
  DWORD written = 0;
  if (logOutput && logOutput != INVALID_HANDLE_VALUE) {
    WriteFile(logOutput, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  }
  return 1;
}

}  // namespace

// --- the System V entry points (va_list forms) ------------------------------

extern "C" {

int BIONIC_SYSV gesher_bionic_vsnprintf(char* buf, size_t n, const char* fmt, SysvVaList* ap) {
  bool valid;
  std::string s = Format(fmt, ap, &valid);
  if (!valid) return -1;
  if (n != 0) {
    size_t c = s.size() < n - 1 ? s.size() : n - 1;
    std::memcpy(buf, s.data(), c);
    buf[c] = 0;
  }
  return static_cast<int>(s.size());
}
int BIONIC_SYSV gesher_bionic_vsprintf(char* buf, const char* fmt, SysvVaList* ap) {
  bool valid;
  std::string s = Format(fmt, ap, &valid);
  if (!valid) return -1;
  std::memcpy(buf, s.c_str(), s.size() + 1);
  return static_cast<int>(s.size());
}
int BIONIC_SYSV gesher_bionic_vfprintf(void* f, const char* fmt, SysvVaList* ap) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return -1;
  if (Orient(stream, -1) == 0) return -1;
  bool valid;
  std::string s = Format(fmt, ap, &valid);
  if (!valid) return -1;
  return gesher::stdio::Write(s.data(), 1, s.size(), stream, true) == s.size() ? static_cast<int>(s.size()) : -1;
}
int BIONIC_SYSV gesher_bionic_vprintf(const char* fmt, SysvVaList* ap) {
  return gesher_bionic_vfprintf(stdout, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vdprintf(int fd, const char* fmt, SysvVaList* ap) {
  bool valid;
  std::string s = Format(fmt, ap, &valid);
  if (!valid) return -1;
  return write(fd, s.data(), static_cast<unsigned>(s.size()));
}
int BIONIC_SYSV gesher_bionic_vasprintf(char** out, const char* fmt, SysvVaList* ap) {
  bool valid;
  std::string s = Format(fmt, ap, &valid);
  if (!valid) { *out = nullptr; return -1; }
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
  bool valid;
  std::string s = Format(fmt, ap, &valid);
  if (!valid) return -1;
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
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return EOF;
  if (Orient(stream, -1) == 0) return EOF;
  return VFscanfImpl(stream, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vscanf(const char* fmt, SysvVaList* ap) {
  return gesher_bionic_vfscanf(stdin, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vswprintf(uint32_t* buf, size_t n, const uint32_t* fmt,
                                       SysvVaList* ap) {
  if (n == 0) return errno = kEINVAL, -1;
  try {
    std::string bytes;
    bytes.reserve(128);
    WideSink sink{&bytes, [](void* context, uint32_t wc) {
      char encoded[4];
      size_t size = EncodeWide(encoded, wc);
      if (size == static_cast<size_t>(-1)) return false;
      static_cast<std::string*>(context)->append(encoded, size);
      return true;
    }};
    bool valid;
    Format(fmt, ap, &valid, &sink);
    if (!valid) return -1;
    if (sink.count == 0) { buf[0] = 0; return 0; }
    const char* input = bytes.c_str();
    size_t decoded = DecodeWideBuffer(buf, &input, n);
    if (decoded == static_cast<size_t>(-1)) return errno = kEilseq, -1;
    if (decoded == n) {
      buf[n - 1] = 0;
      return errno = kEoverflow, -1;
    }
    // Android returns the formatting count, even if an embedded NUL ended
    // the subsequent mbsrtowcs conversion before that many characters.
    return static_cast<int>(sink.count);
  } catch (const std::bad_alloc&) {
    return errno = kENOMEM, -1;
  } catch (const std::length_error&) {
    return errno = kENOMEM, -1;
  }
}
int BIONIC_SYSV gesher_bionic_vfwprintf(void* f, const uint32_t* fmt, SysvVaList* ap) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (!lock) return -1;
  StreamState* state = nullptr;
  try {
    state = lock.Metadata();
    if (!state || Orient(stream, 1) == 0) return -1;
    struct Target { FILE* stream; StreamState* state; } target{stream, state};
    WideSink sink{&target, [](void* context, uint32_t wc) {
      auto* target = static_cast<Target*>(context);
      return WriteWide(target->stream, *target->state, wc);
    }};
    bool valid;
    Format(fmt, ap, &valid, &sink);
    if (!valid) {
      if (errno == kEilseq) state->conversion_error = true;
      return -1;
    }
    return static_cast<int>(sink.count);
  } catch (const std::bad_alloc&) {
    if (state) state->conversion_error = true;
    return errno = kENOMEM, -1;
  } catch (const std::length_error&) {
    if (state) state->conversion_error = true;
    return errno = kENOMEM, -1;
  }
}
int BIONIC_SYSV gesher_bionic_vwprintf(const uint32_t* fmt, SysvVaList* ap) {
  return gesher_bionic_vfwprintf(stdout, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_android_log_vprint(int prio, const char* tag, const char* fmt,
                                                SysvVaList* ap) {
  bool valid;
  std::string text = Format(fmt, ap, &valid);
  return valid ? LogWrite(prio, tag, text.c_str()) : -1;
}
int BIONIC_SYSV gesher_bionic_android_log_buf_vprint(int buf, int prio, const char* tag,
                                                    const char* fmt, SysvVaList* ap) {
  bool valid;
  std::string text = Format(fmt, ap, &valid);
  return valid ? LogWrite(prio, tag, text.c_str(), buf) : -1;
}
void BIONIC_SYSV gesher_bionic_android_log_vassert(const char* cond, const char* tag,
                                                  const char* fmt, SysvVaList* ap) {
  bool valid = false;
  std::string text;
  if (fmt != nullptr) text = Format(fmt, ap, &valid);
  if (valid) {
    LogWrite(7, tag, text.c_str(), 0, false);
  } else {
    LogWrite(7, tag, cond ? cond : "assertion failed", 0, false);
  }
  std::abort();
}
void BIONIC_SYSV gesher_bionic_vsyslog(int prio, const char* fmt, SysvVaList* ap) {
  bool valid;
  std::string text = Format(fmt, ap, &valid);
  if (valid) LogWrite(prio <= 3 ? 6 : prio <= 4 ? 5 : prio <= 6 ? 4 : 3, "syslog", text.c_str());
}
int BIONIC_SYSV gesher_bionic_android_log_write(int prio, const char* tag, const char* text) {
  return LogWrite(prio, tag, text);
}
int BIONIC_SYSV gesher_bionic_android_log_buf_write(int buf, int prio, const char* tag,
                                                   const char* text) {
  return LogWrite(prio, tag, text, buf);
}
int BIONIC_SYSV gesher_bionic_android_log_is_loggable_len(int prio, const char* tag, size_t length, int fallback) {
  return ::__android_log_is_loggable_len(prio, tag, length, fallback);
}
int BIONIC_SYSV gesher_bionic_android_log_is_loggable(int prio, const char* tag, int fallback) {
  return gesher_bionic_android_log_is_loggable_len(prio, tag, tag ? std::strlen(tag) : 0, fallback);
}
int32_t BIONIC_SYSV gesher_bionic_android_log_get_minimum_priority() { return ::__android_log_get_minimum_priority(); }
int32_t BIONIC_SYSV gesher_bionic_android_log_set_minimum_priority(int32_t priority) {
  return ::__android_log_set_minimum_priority(priority);
}
void BIONIC_SYSV gesher_bionic_android_log_set_default_tag(const char* tag) {
  ::__android_log_set_default_tag(tag);
}

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
void gesher_bionic_fwprintf();
void gesher_bionic_wprintf();
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
    GESHER_VA_TRAMPOLINE("gesher_bionic_fwprintf", "gesher_bionic_vfwprintf", "16", "%rdx")
    GESHER_VA_TRAMPOLINE("gesher_bionic_wprintf", "gesher_bionic_vwprintf", "8", "%rsi")
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

// fts(3) directory-tree walks: no FTS here (its FTSENT carries the caller's
// struct stat, which differs between x86_64 and a translated arm64 guest).
// fts_open fails as an unreadable tree does, so a caller that walks a tree
// (libtaz_full.so's integrity checks) sees an error instead of an abort.
void* BIONIC_SYSV gesher_bionic_fts_open(char* const*, int, void*) {
  errno = kENOSYS;
  return nullptr;
}
void* BIONIC_SYSV gesher_bionic_fts_read(void*) {
  errno = kENOSYS;
  return nullptr;
}
int BIONIC_SYSV gesher_bionic_fts_close(void*) { return 0; }
int BIONIC_SYSV gesher_bionic_fts_set(void*, void*, int) { return 0; }

void RegisterStdio(SymbolTable& t) {
  // Validate during shim initialization so even the first try-lock avoids
  // opening a CRT probe stream on the application's nonblocking call path.
  stdio_windows::StreamLockLayoutSupported();
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
  E("fwprintf", gesher_bionic_fwprintf);
  E("wprintf", gesher_bionic_wprintf);
  E("vfwprintf", gesher_bionic_vfwprintf);
  E("vwprintf", gesher_bionic_vwprintf);
  E("vswprintf", gesher_bionic_vswprintf);
  E("syslog", gesher_bionic_syslog);
  E("vsyslog", gesher_bionic_vsyslog);
  E("fts_open", gesher_bionic_fts_open);
  E("fts_read", gesher_bionic_fts_read);
  E("fts_children", gesher_bionic_fts_read);
  E("fts_close", gesher_bionic_fts_close);
  E("fts_set", gesher_bionic_fts_set);
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
  E("getw", Getw);
  E("putw", Putw);
  E("flockfile", Flockfile);
  E("funlockfile", Funlockfile);
  E("ftrylockfile", Ftrylockfile);
  E("fgetws", Fgetws);
  E("getc_unlocked", GetcUnlocked);
  E("fgetc_unlocked", GetcUnlocked);
  E("getchar_unlocked", GetcharUnlocked);
  E("putc_unlocked", PutcUnlocked);
  E("fputc_unlocked", PutcUnlocked);
  E("putchar_unlocked", PutcharUnlocked);
  E("fread_unlocked", FreadUnlocked);
  E("fwrite_unlocked", FwriteUnlocked);
  E("fgets_unlocked", FgetsUnlocked);
  E("fputs_unlocked", FputsUnlocked);
  E("ferror_unlocked", FerrorUnlocked);
  E("clearerr_unlocked", ClearerrUnlocked);
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
  E("__android_log_is_loggable_len", gesher_bionic_android_log_is_loggable_len);
  E("__android_log_get_minimum_priority", gesher_bionic_android_log_get_minimum_priority);
  E("__android_log_set_minimum_priority", gesher_bionic_android_log_set_minimum_priority);
  E("__android_log_set_default_tag", gesher_bionic_android_log_set_default_tag);
#undef E
}

}  // namespace gesher::bionic
