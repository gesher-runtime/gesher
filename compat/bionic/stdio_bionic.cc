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
#include "stdio_windows_lock.h"
#include <gesher/bionic_shim.h>
#include <gesher/logging.h>

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

using WideText = std::vector<uint32_t>;

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
FormatText<Char> Format(const Char* fmt, SysvVaList* ap, bool* valid = nullptr) {
  constexpr bool wide = !std::is_same_v<Char, char>;
  FormatText<Char> out;
  if (valid) *valid = true;
  const Char* p = fmt;
  while (*p != 0) {
    if (*p != '%') {
      const Char* begin = p;
      if constexpr (wide) {
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
    std::string flags;
    while (*p > 0 && *p <= 0x7f && std::strchr("-+ #0'", *p) != nullptr) {
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
    if (*p == 0) {
      out.insert(out.end(), start, p);
      break;
    }
    Char conversion = *p++;
    if (conversion < 0 || conversion > 0x7f) {
      out.insert(out.end(), start, p);
      continue;
    }
    char conv = static_cast<char>(conversion);

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
            if (byte >= 0x80) {
              errno = kEilseq;
              *valid = false;
              return {};
            }
            wc = byte;
          }
          AppendWideField(&out, {wc}, width, flags.find('-') != std::string::npos);
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
            while ((prec < 0 || text.size() < static_cast<size_t>(prec)) && *ws != 0)
              text.push_back(*ws++);
          } else if (!DecodeWideString(static_cast<const char*>(ptr), prec, &text)) {
            *valid = false;
            return {};
          }
          AppendWideField(&out, text, width, flags.find('-') != std::string::npos);
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
        out.insert(out.end(), start, p);
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

// Stream metadata is used only while the caller owns the CRT's FILE lock,
// or supplies the synchronization required by an unlocked stdio API. The
// registry mutex protects insert/erase and is released before doing any I/O.
constexpr uint32_t kWeof = 0xffffffffu;
struct StreamState {
  int orientation = 0;
  bool conversion_error = false;
  uint32_t pushback = kWeof;
  std::array<uint8_t, 8> input_state{};  // Bionic LP64 mbstate_t
};
struct StreamStateBucket {
  std::mutex mutex;
  std::unordered_map<FILE*, StreamState> streams;
};
std::array<StreamStateBucket, 32> g_stream_states;

StreamStateBucket& StreamBucket(FILE* stream) {
  auto key = reinterpret_cast<uintptr_t>(stream);
  return g_stream_states[(key >> 4) % g_stream_states.size()];
}

StreamState& State(FILE* stream) {
  auto& bucket = StreamBucket(stream);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  return bucket.streams[stream];
}

int Orient(StreamState& state, int mode) {
  if (state.orientation == 0 && mode != 0) state.orientation = mode > 0 ? 1 : -1;
  return state.orientation;
}

void ResetStream(FILE* stream) {
  auto& bucket = StreamBucket(stream);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  bucket.streams.erase(stream);
}

class StreamLock {
 public:
  explicit StreamLock(FILE* stream) : stream_(stream) { _lock_file(stream_); }
  ~StreamLock() { _unlock_file(stream_); }
  StreamLock(const StreamLock&) = delete;
  StreamLock& operator=(const StreamLock&) = delete;

 private:
  FILE* stream_;
};

void BIONIC_SYSV Flockfile(void* f) { _lock_file(F(f)); }
void BIONIC_SYSV Funlockfile(void* f) { _unlock_file(F(f)); }
int BIONIC_SYSV Ftrylockfile(void* f) {
  switch (stdio_windows::TryLock(F(f))) {
    case stdio_windows::TryLockResult::kAcquired: return 0;
    case stdio_windows::TryLockResult::kBusy: return kEBUSY;
    case stdio_windows::TryLockResult::kUnsupported: return kEOPNOTSUPP;
  }
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
}  // namespace

int CloseBionicStream(FILE* stream) {
  // UCRT fclose marks its stream slot reusable. Allocation takes its lock
  // before reusing it; the lock persists until CRT shutdown or fcloseall.
  // Keep our state removal in the same transaction as Gesher POSIX fclose.
  StreamLock lock(stream);
  ResetStream(stream);
  return std::fclose(stream);
}

namespace {

// /dev/zero and /dev/random are NUL handles that gesher's read() fills: the
// CRT's own read of one returns nothing (mbedtls seeds its DRBG with
// fread from fopen("/dev/urandom"), and WhatsApp aborts when that fails).
size_t ReadStream(void* buf, size_t size, size_t n, FILE* f) {
  int fd = _fileno(f);
  if (size == 0 || n == 0 || fd < 0 || !gesher_is_device_fd(fd)) {
    return _fread_nolock(buf, size, n, f);
  }
  auto* p = static_cast<unsigned char*>(buf);
  size_t total = n > SIZE_MAX / size ? SIZE_MAX / size * size : size * n;
  size_t done = 0;
  while (done < total) {
    size_t chunk = total - done < 0x40000000 ? total - done : 0x40000000;
    int r = read(fd, p + done, static_cast<unsigned int>(chunk));
    if (r <= 0) break;
    done += static_cast<size_t>(r);
  }
  return done / size;
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
  if (m.empty()) {
    CloseBionicStream(F(f));
    return errno = kEINVAL, nullptr;
  }
  // gesher-base's freopen consumes Linux modes and maps them for the CRT.
  // Passing CrtMode's 'N' would lose Android's 'e' before that mapping.
  FILE* stream = F(f);
  StreamLock lock(stream);
  FILE* reopened = std::freopen(path, mode, stream);
  // freopen closes the original stream on failure as well. The host's
  // successful null-path mode-change stand-in still returns the same stream.
  ResetStream(stream);
  return reopened;
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
  StreamLock lock(fp);
  Orient(State(fp), -1);
  size_t len = 0;
  int c;
  while ((c = _getc_nolock(fp)) != EOF) {
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

uint32_t ReadWide(FILE* stream, StreamState& state) {
  if (state.pushback != kWeof) {
    uint32_t wc = state.pushback;
    state.pushback = kWeof;
    return wc;
  }
  while (true) {
    int byte = _getc_nolock(stream);
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
  return _fwrite_nolock(bytes, 1, size, stream) == size;
}

uint32_t BIONIC_SYSV Fgetwc(void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  auto& state = State(stream);
  Orient(state, 1);
  return ReadWide(stream, state);
}

uint32_t* BIONIC_SYSV Fgetws(uint32_t* out, int n, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  auto& state = State(stream);
  Orient(state, 1);
  if (n <= 0) return errno = kEINVAL, nullptr;
  int count = 0;
  while (count < n - 1) {
    uint32_t wc = ReadWide(stream, state);
    if (wc == kWeof) {
      if (count == 0 || (errno == kEilseq && (state.conversion_error || std::ferror(stream))))
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
  auto& state = State(stream);
  Orient(state, 1);
  if (state.pushback != kWeof) return kWeof;
  state.pushback = c;
  state.conversion_error = false;
  std::clearerr(stream);
  return c;
}

uint32_t BIONIC_SYSV Fputwc(uint32_t c, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  auto& state = State(stream);
  Orient(state, 1);
  return WriteWide(stream, state, c) ? c : kWeof;
}

int BIONIC_SYSV Fputws(const uint32_t* ws, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  auto& state = State(stream);
  Orient(state, 1);
  for (; *ws != 0; ++ws) {
    if (!WriteWide(stream, state, *ws)) return -1;
  }
  return 0;
}

uint32_t BIONIC_SYSV Getwchar() { return Fgetwc(stdin); }
uint32_t BIONIC_SYSV Putwchar(uint32_t c) { return Fputwc(c, stdout); }
int BIONIC_SYSV Fwide(void* f, int mode) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  return Orient(State(stream), mode);
}
int BIONIC_SYSV Fclose(void* f) { return CloseBionicStream(F(f)); }

int BIONIC_SYSV GetcUnlocked(void* f) {
  FILE* stream = F(f);
  Orient(State(stream), -1);
  unsigned char c;
  int fd = _fileno(stream);
  if (fd >= 0 && gesher_is_device_fd(fd)) return ReadStream(&c, 1, 1, stream) == 1 ? c : EOF;
  return _getc_nolock(stream);
}
int BIONIC_SYSV PutcUnlocked(int c, void* f) {
  FILE* stream = F(f);
  Orient(State(stream), -1);
  return _putc_nolock(c, stream);
}
int BIONIC_SYSV GetcharUnlocked() { return GetcUnlocked(stdin); }
int BIONIC_SYSV PutcharUnlocked(int c) { return PutcUnlocked(c, stdout); }
size_t BIONIC_SYSV FreadUnlocked(void* buf, size_t size, size_t n, void* f) {
  FILE* stream = F(f);
  if (size != 0 && n != 0) Orient(State(stream), -1);
  return ReadStream(buf, size, n, stream);
}
size_t BIONIC_SYSV FwriteUnlocked(const void* buf, size_t size, size_t n, void* f) {
  FILE* stream = F(f);
  if (size != 0 && n != 0) Orient(State(stream), -1);
  return _fwrite_nolock(buf, size, n, stream);
}
int BIONIC_SYSV FputsUnlocked(const char* s, void* f) {
  FILE* stream = F(f);
  Orient(State(stream), -1);
  size_t n = std::strlen(s);
  return _fwrite_nolock(s, 1, n, stream) == n ? 0 : EOF;
}
char* BIONIC_SYSV FgetsUnlocked(char* s, int n, void* f) {
  if (n <= 0) return errno = kEINVAL, nullptr;
  FILE* stream = F(f);
  Orient(State(stream), -1);
  int count = 0;
  while (count < n - 1) {
    int c = _getc_nolock(stream);
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
  StreamLock lock(F(f));
  return FreadUnlocked(buf, size, n, f);
}
size_t BIONIC_SYSV Fwrite(const void* buf, size_t size, size_t n, void* f) {
  StreamLock lock(F(f));
  return FwriteUnlocked(buf, size, n, f);
}
int BIONIC_SYSV Fputs(const char* s, void* f) {
  StreamLock lock(F(f));
  return FputsUnlocked(s, f);
}
int BIONIC_SYSV Fputc(int c, void* f) {
  StreamLock lock(F(f));
  return PutcUnlocked(c, f);
}
int BIONIC_SYSV Putchar(int c) { return Fputc(c, stdout); }
int BIONIC_SYSV Puts(const char* s) {
  StreamLock lock(stdout);
  Orient(State(stdout), -1);
  return std::puts(s);
}
char* BIONIC_SYSV Fgets(char* s, int n, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  if (n > 0) Orient(State(stream), -1);
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
  return GetcUnlocked(f);
}
int BIONIC_SYSV Getchar() { return Fgetc(stdin); }
int BIONIC_SYSV Ungetc(int c, void* f) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  int result = _ungetc_nolock(c, stream);
  if (result != EOF) Orient(State(stream), -1);
  return result;
}
int BIONIC_SYSV Fflush(void* f) { return std::fflush(f ? F(f) : nullptr); }
int BIONIC_SYSV Feof(void* f) { return std::feof(F(f)); }
int BIONIC_SYSV FerrorUnlocked(void* f) {
  FILE* stream = F(f);
  return State(stream).conversion_error || std::ferror(stream);
}
int BIONIC_SYSV Ferror(void* f) {
  StreamLock lock(F(f));
  return FerrorUnlocked(f);
}
void BIONIC_SYSV ClearerrUnlocked(void* f) {
  FILE* stream = F(f);
  State(stream).conversion_error = false;
  std::clearerr(stream);
}
void BIONIC_SYSV Clearerr(void* f) {
  StreamLock lock(F(f));
  ClearerrUnlocked(f);
}
int BIONIC_SYSV Fileno(void* f) { return _fileno(F(f)); }
int BIONIC_SYSV Fseek(void* f, int64_t off, int whence) { return _fseeki64(F(f), off, whence); }
int64_t BIONIC_SYSV Ftell(void* f) { return _ftelli64(F(f)); }
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
  _fseeki64_nolock(stream, 0, SEEK_SET);
  // Like Android's rewind, clear stream errors even if seeking failed.
  ClearerrUnlocked(f);
}
int BIONIC_SYSV Setvbuf(void* f, char* buf, int mode, size_t size) {
  // bionic: _IOFBF 0, _IOLBF 1, _IONBF 2; the CRT: 0x0000, 0x0040, 0x0004.
  static const int kModes[] = {_IOFBF, _IOLBF, _IONBF};
  if (mode < 0 || mode > 2) return -1;
  return std::setvbuf(F(f), buf, kModes[mode], size);
}
void BIONIC_SYSV Setbuf(void* f, char* buf) { std::setbuf(F(f), buf); }
FILE* BIONIC_SYSV Tmpfile() { return std::tmpfile(); }
void BIONIC_SYSV Perror(const char* s) {
  StreamLock lock(stderr);
  Orient(State(stderr), -1);
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
  FILE* stream = F(f);
  StreamLock lock(stream);
  Orient(State(stream), -1);
  std::string s = Format(fmt, ap);
  return _fwrite_nolock(s.data(), 1, s.size(), stream) == s.size() ? static_cast<int>(s.size()) : -1;
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
  FILE* stream = F(f);
  StreamLock lock(stream);
  Orient(State(stream), -1);
  return VFscanfImpl(stream, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vscanf(const char* fmt, SysvVaList* ap) {
  return gesher_bionic_vfscanf(stdin, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_vswprintf(uint32_t* buf, size_t n, const uint32_t* fmt,
                                       SysvVaList* ap) {
  if (n == 0) return -1;
  bool valid;
  WideText text = Format(fmt, ap, &valid);
  if (!valid) return -1;
  size_t count = text.size() < n - 1 ? text.size() : n - 1;
  if (count != 0) std::memcpy(buf, text.data(), count * sizeof(uint32_t));
  buf[count] = 0;
  if (text.size() > INT_MAX) return errno = kEoverflow, -1;
  return text.size() < n ? static_cast<int>(text.size()) : -1;
}
int BIONIC_SYSV gesher_bionic_vfwprintf(void* f, const uint32_t* fmt, SysvVaList* ap) {
  FILE* stream = F(f);
  StreamLock lock(stream);
  auto& state = State(stream);
  Orient(state, 1);
  bool valid;
  WideText text = Format(fmt, ap, &valid);
  if (!valid) {
    state.conversion_error = true;
    return -1;
  }
  if (text.size() > INT_MAX) return errno = kEoverflow, -1;
  for (uint32_t wc : text) {
    if (!WriteWide(stream, state, wc)) return -1;
  }
  return static_cast<int>(text.size());
}
int BIONIC_SYSV gesher_bionic_vwprintf(const uint32_t* fmt, SysvVaList* ap) {
  return gesher_bionic_vfwprintf(stdout, fmt, ap);
}
int BIONIC_SYSV gesher_bionic_android_log_vprint(int prio, const char* tag, const char* fmt,
                                                SysvVaList* ap) {
  return LogWrite(prio, tag, Format(fmt, ap).c_str());
}
int BIONIC_SYSV gesher_bionic_android_log_buf_vprint(int buf, int prio, const char* tag,
                                                    const char* fmt, SysvVaList* ap) {
  return LogWrite(prio, tag, Format(fmt, ap).c_str(), buf);
}
void BIONIC_SYSV gesher_bionic_android_log_vassert(const char* cond, const char* tag,
                                                  const char* fmt, SysvVaList* ap) {
  if (fmt != nullptr) {
    LogWrite(7, tag, Format(fmt, ap).c_str(), 0, false);
  } else {
    LogWrite(7, tag, cond ? cond : "assertion failed", 0, false);
  }
  std::abort();
}
void BIONIC_SYSV gesher_bionic_vsyslog(int prio, const char* fmt, SysvVaList* ap) {
  LogWrite(prio <= 3 ? 6 : prio <= 4 ? 5 : prio <= 6 ? 4 : 3, "syslog", Format(fmt, ap).c_str());
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
