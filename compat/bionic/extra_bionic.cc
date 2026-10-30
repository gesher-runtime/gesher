// gesher bionic shim: libc/libm functions beyond the core files, which real
// apps' NDK libraries and popular NDK libraries import (docs/native-libs.md,
// ADR 0057). The arm64 translator used to carry some of them as guest-only
// fallbacks (sem_*, the 32-bit wchar_t functions, libm extras); they are here
// now, so x86-64 libraries get them too and arm64 code reaches them through
// the shim like everything else.
//
// RegisterExtra runs before every other Register*: a name the core files
// define keeps their definition.
//
// This file: libm (and the x86-64 floating-point environment), stdlib
// (div, rand48, getopt, mkstemp, pwd/grp), strings and locale-suffixed forms,
// wide characters and wide stdio, unlocked stdio, strptime, system
// properties, and the registry of platform libraries for dlopen.
// extra_sys_bionic.cc: semaphores, scheduling, pthread additions, resource
// usage and time, Linux system calls, flock. zlib_bionic.cc: libz.so.

#include "extra_bionic.h"
#include "unicode_bionic.h"

#include <gesher/bionic_shim.h>
#include <gesher/system_properties.h>

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
// stdio_bionic.cc's va_list forms (System V, extern "C" for its trampolines).
int __attribute__((sysv_abi)) gesher_bionic_vfprintf(void* f, const char* fmt,
                                                     gesher::bionic::SysvVaList* ap);
}

namespace gesher::bionic {

namespace {

// A sysv_abi wrapper around a function whose signature means the same to the
// .so, as in shim.cc.
template <auto F>
struct Sysv;
template <class R, class... A, R (*F)(A...)>
struct Sysv<F> {
  static R BIONIC_SYSV Call(A... args) { return F(args...); }
};
#define WRAPS(sig, fn) reinterpret_cast<void*>(&Sysv<static_cast<sig>(&fn)>::Call)
#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)

// Another part of the shim's entry point, looked up when first called (the
// table is complete by then).
template <class Fn>
Fn Shim(const char* name) {
  return reinterpret_cast<Fn>(LookupBionicSymbol(name));
}

using wchar32 = uint32_t;

// --- libm ----------------------------------------------------------------------

double BIONIC_SYSV Scalbln(double x, int64_t n) {
  return std::scalbn(x, static_cast<int>(n > 100000 ? 100000 : n < -100000 ? -100000 : n));
}
float BIONIC_SYSV Scalblnf(float x, int64_t n) {
  return std::scalbn(x, static_cast<int>(n > 100000 ? 100000 : n < -100000 ? -100000 : n));
}
double BIONIC_SYSV Nan(const char*) { return std::nan(""); }
float BIONIC_SYSV Nanf(const char*) { return std::nanf(""); }
int GammaSign(double x) {
  if (x > 0 || x != x) return 1;
  double f = std::floor(x);
  if (f == x) return 1;  // a pole
  return (static_cast<int64_t>(f) & 1) != 0 ? -1 : 1;
}
double BIONIC_SYSV LgammaR(double x, int* sign) {
  if (sign) *sign = GammaSign(x);
  return std::lgamma(x);
}
float BIONIC_SYSV LgammafR(float x, int* sign) {
  if (sign) *sign = GammaSign(x);
  return std::lgamma(x);
}
double BIONIC_SYSV Significand(double x) {
  if (x == 0 || !std::isfinite(x)) return x;
  return std::scalbn(x, -std::ilogb(x));
}
float BIONIC_SYSV Significandf(float x) {
  if (x == 0 || !std::isfinite(x)) return x;
  return std::scalbn(x, -std::ilogb(x));
}
double BIONIC_SYSV Exp10(double x) { return std::pow(10.0, x); }
float BIONIC_SYSV Exp10f(float x) { return std::pow(10.0f, x); }
double BIONIC_SYSV J0(double x) { return ::_j0(x); }
double BIONIC_SYSV J1(double x) { return ::_j1(x); }
double BIONIC_SYSV Jn(int n, double x) { return ::_jn(n, x); }
double BIONIC_SYSV Y0(double x) { return ::_y0(x); }
double BIONIC_SYSV Y1(double x) { return ::_y1(x); }
double BIONIC_SYSV Yn(int n, double x) { return ::_yn(n, x); }
float BIONIC_SYSV J0f(float x) { return static_cast<float>(::_j0(x)); }
float BIONIC_SYSV J1f(float x) { return static_cast<float>(::_j1(x)); }
float BIONIC_SYSV Jnf(int n, float x) { return static_cast<float>(::_jn(n, x)); }
float BIONIC_SYSV Y0f(float x) { return static_cast<float>(::_y0(x)); }
float BIONIC_SYSV Y1f(float x) { return static_cast<float>(::_y1(x)); }
float BIONIC_SYSV Ynf(int n, float x) { return static_cast<float>(::_yn(n, x)); }
int BIONIC_SYSV IsNan(double x) { return std::isnan(x) ? 1 : 0; }
int BIONIC_SYSV IsNanf(float x) { return std::isnan(x) ? 1 : 0; }
int BIONIC_SYSV IsInf(double x) { return std::isinf(x) ? (std::signbit(x) ? -1 : 1) : 0; }
int BIONIC_SYSV IsInff(float x) { return std::isinf(x) ? (std::signbit(x) ? -1 : 1) : 0; }
int BIONIC_SYSV Finite(double x) { return std::isfinite(x) ? 1 : 0; }
int BIONIC_SYSV Finitef(float x) { return std::isfinite(x) ? 1 : 0; }
int BIONIC_SYSV IsNormal(double x) { return std::isnormal(x) ? 1 : 0; }
int BIONIC_SYSV IsNormalf(float x) { return std::isnormal(x) ? 1 : 0; }
int BIONIC_SYSV SignBit(double x) { return std::signbit(x) ? 1 : 0; }
int BIONIC_SYSV SignBitf(float x) { return std::signbit(x) ? 1 : 0; }
// bionic's (FreeBSD's) FP_* values.
int Classify(int c) {
  switch (c) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;  // FP_ZERO
  }
}
int BIONIC_SYSV Fpclassify(double x) { return Classify(std::fpclassify(x)); }
int BIONIC_SYSV Fpclassifyf(float x) { return Classify(std::fpclassify(x)); }

// The x86-64 floating-point environment: bionic's fenv_t is the x87
// environment (fnstenv's 28 bytes, padded to 28) and MXCSR. FE_* values are
// x86's, the same as the CRT's. (The arm64 translator has its own: guest_libc.cc.)
struct X64Fenv {
  uint32_t control, status, tag;
  char other[16];
  uint32_t mxcsr;
};
static_assert(sizeof(X64Fenv) == 32, "bionic x86-64 fenv_t");
const X64Fenv kDefaultFenv = {0x037f, 0, 0xffff, {}, 0x1f80};
constexpr int kFeAll = 0x3d;  // FE_INVALID | DIVBYZERO | OVERFLOW | UNDERFLOW | INEXACT (| DENORMAL 2)

int BIONIC_SYSV Fegetenv(X64Fenv* env) {
  __asm__ volatile("fnstenv %0" : "=m"(*env));
  __asm__ volatile("fldenv %0" : : "m"(*env));  // fnstenv masks exceptions: restore
  __asm__ volatile("stmxcsr %0" : "=m"(env->mxcsr));
  return 0;
}
int BIONIC_SYSV Fesetenv(const X64Fenv* env) {
  if (env == nullptr) env = &kDefaultFenv;
  X64Fenv e = *env;
  __asm__ volatile("fldenv %0" : : "m"(e));
  __asm__ volatile("ldmxcsr %0" : : "m"(e.mxcsr));
  return 0;
}
int BIONIC_SYSV Fegetround() {
  uint32_t m;
  __asm__ volatile("stmxcsr %0" : "=m"(m));
  return static_cast<int>((m >> 3) & 0xc00);
}
int BIONIC_SYSV Fesetround(int mode) {
  if ((mode & ~0xc00) != 0) return -1;
  uint16_t cw;
  __asm__ volatile("fnstcw %0" : "=m"(cw));
  cw = static_cast<uint16_t>((cw & ~0xc00) | mode);
  __asm__ volatile("fldcw %0" : : "m"(cw));
  uint32_t m;
  __asm__ volatile("stmxcsr %0" : "=m"(m));
  m = (m & ~0x6000u) | (static_cast<uint32_t>(mode) << 3);
  __asm__ volatile("ldmxcsr %0" : : "m"(m));
  return 0;
}
int BIONIC_SYSV Fetestexcept(int excepts) {
  uint16_t sw;
  uint32_t m;
  __asm__ volatile("fnstsw %0" : "=m"(sw));
  __asm__ volatile("stmxcsr %0" : "=m"(m));
  return (sw | m) & excepts & kFeAll;
}
int BIONIC_SYSV Feclearexcept(int excepts) {
  X64Fenv env;
  __asm__ volatile("fnstenv %0" : "=m"(env));
  env.status &= ~static_cast<uint32_t>(excepts & kFeAll);
  __asm__ volatile("fldenv %0" : : "m"(env));
  uint32_t m;
  __asm__ volatile("stmxcsr %0" : "=m"(m));
  m &= ~static_cast<uint32_t>(excepts & kFeAll);
  __asm__ volatile("ldmxcsr %0" : : "m"(m));
  return 0;
}
int BIONIC_SYSV Feraiseexcept(int excepts) {
  uint32_t m;
  __asm__ volatile("stmxcsr %0" : "=m"(m));
  m |= static_cast<uint32_t>(excepts & kFeAll);
  __asm__ volatile("ldmxcsr %0" : : "m"(m));
  return 0;
}
int BIONIC_SYSV Feholdexcept(X64Fenv* env) {
  Fegetenv(env);
  X64Fenv e = *env;
  e.status &= ~static_cast<uint32_t>(kFeAll);
  e.control |= kFeAll;  // non-stop: every exception masked
  e.mxcsr = (e.mxcsr & ~static_cast<uint32_t>(kFeAll)) | (static_cast<uint32_t>(kFeAll) << 7);
  Fesetenv(&e);
  return 0;
}
int BIONIC_SYSV Feupdateenv(const X64Fenv* env) {
  int raised = Fetestexcept(kFeAll);
  Fesetenv(env);
  Feraiseexcept(raised);
  return 0;
}
int BIONIC_SYSV Fegetexceptflag(uint16_t* flag, int excepts) {
  *flag = static_cast<uint16_t>(Fetestexcept(excepts));
  return 0;
}
int BIONIC_SYSV Fesetexceptflag(const uint16_t* flag, int excepts) {
  Feclearexcept(excepts);
  Feraiseexcept(*flag & excepts);
  return 0;
}
int BIONIC_SYSV Feenableexcept(int) { return -1; }  // traps are not supported
int BIONIC_SYSV Fedisableexcept(int) { return 0; }
int BIONIC_SYSV Fegetexcept() { return 0; }

// --- stdlib ----------------------------------------------------------------------

struct DivInt {
  int quot, rem;
};
struct DivLong {
  int64_t quot, rem;
};
DivInt BIONIC_SYSV Div(int a, int b) { return {a / b, a % b}; }
DivLong BIONIC_SYSV Ldiv(int64_t a, int64_t b) { return {a / b, a % b}; }
int64_t BIONIC_SYSV Imaxabs(int64_t v) { return v < 0 ? -v : v; }

int BIONIC_SYSV RandR(unsigned* seed) {
  *seed = *seed * 1103515245u + 12345u;
  return static_cast<int>((*seed >> 1) & 0x7fffffff);
}

// rand48: Linux's 48-bit generator.
uint16_t g_rand48[3] = {0x330e, 0xabcd, 0x1234};
uint16_t g_rand48_mult[3] = {0xe66d, 0xdeec, 0x0005};
uint16_t g_rand48_add = 0x000b;
std::mutex g_rand48_mutex;
uint64_t Next48(uint16_t x[3]) {
  uint64_t v = static_cast<uint64_t>(x[0]) | (static_cast<uint64_t>(x[1]) << 16) |
               (static_cast<uint64_t>(x[2]) << 32);
  uint64_t a = static_cast<uint64_t>(g_rand48_mult[0]) | (static_cast<uint64_t>(g_rand48_mult[1]) << 16) |
               (static_cast<uint64_t>(g_rand48_mult[2]) << 32);
  v = (v * a + g_rand48_add) & 0xffffffffffffULL;
  x[0] = static_cast<uint16_t>(v);
  x[1] = static_cast<uint16_t>(v >> 16);
  x[2] = static_cast<uint16_t>(v >> 32);
  return v;
}
double BIONIC_SYSV Erand48(uint16_t x[3]) {
  std::lock_guard<std::mutex> lock(g_rand48_mutex);
  return std::ldexp(static_cast<double>(Next48(x)), -48);
}
double BIONIC_SYSV Drand48() { return Erand48(g_rand48); }
int64_t BIONIC_SYSV Nrand48(uint16_t x[3]) {
  std::lock_guard<std::mutex> lock(g_rand48_mutex);
  return static_cast<int64_t>(Next48(x) >> 17);
}
int64_t BIONIC_SYSV Jrand48(uint16_t x[3]) {
  std::lock_guard<std::mutex> lock(g_rand48_mutex);
  return static_cast<int64_t>(static_cast<int32_t>(Next48(x) >> 16));
}
int64_t BIONIC_SYSV Mrand48() { return Jrand48(g_rand48); }
uint16_t* BIONIC_SYSV Seed48(const uint16_t s[3]) {
  static uint16_t old[3];
  std::lock_guard<std::mutex> lock(g_rand48_mutex);
  std::memcpy(old, g_rand48, sizeof(old));
  std::memcpy(g_rand48, s, sizeof(g_rand48));
  g_rand48_mult[0] = 0xe66d, g_rand48_mult[1] = 0xdeec, g_rand48_mult[2] = 0x0005;
  g_rand48_add = 0x000b;
  return old;
}
void BIONIC_SYSV Lcong48(const uint16_t p[7]) {
  std::lock_guard<std::mutex> lock(g_rand48_mutex);
  std::memcpy(g_rand48, p, 6);
  std::memcpy(g_rand48_mult, p + 3, 6);
  g_rand48_add = p[6];
}

// getopt/getopt_long/getopt_long_only, GNU-style: options are permuted ahead
// of the operands unless optstring starts with '+' (or POSIXLY_CORRECT is
// set); '-' returns operands as option 1. optarg, optind, opterr, optopt and
// optreset are the shim's data.
char* g_optarg = nullptr;
int g_optind = 1;
int g_opterr = 1;
int g_optopt = '?';
int g_optreset = 0;
int g_first_nonopt = 1, g_last_nonopt = 1;
const char* g_nextchar = nullptr;

struct BionicOption {
  const char* name;
  int has_arg;
  int* flag;
  int val;
};

void Exchange(char** argv) {
  std::rotate(argv + g_first_nonopt, argv + g_last_nonopt, argv + g_optind);
  g_first_nonopt += g_optind - g_last_nonopt;
  g_last_nonopt = g_optind;
}

bool IsOperand(char** argv, int i) { return argv[i][0] != '-' || argv[i][1] == '\0'; }

int GetoptImpl(int argc, char** argv, const char* optstring, const BionicOption* longopts,
               int* longindex, bool long_only) {
  if (optstring == nullptr) optstring = "";
  if (g_optind == 0 || g_optreset) {
    g_optind = 1;
    g_optreset = 0;
    g_nextchar = nullptr;
    g_first_nonopt = g_last_nonopt = 1;
  }
  g_optarg = nullptr;
  enum { kPermute, kRequireOrder, kReturnInOrder } ordering = kPermute;
  if (*optstring == '-') {
    ordering = kReturnInOrder;
    ++optstring;
  } else if (*optstring == '+') {
    ordering = kRequireOrder;
    ++optstring;
  } else if (std::getenv("POSIXLY_CORRECT") != nullptr) {
    ordering = kRequireOrder;
  }
  bool colon = *optstring == ':';
  bool print = g_opterr != 0 && !colon;
  const char* prog = argc > 0 && argv[0] != nullptr ? argv[0] : "";

  if (g_nextchar == nullptr || *g_nextchar == '\0') {
    if (g_last_nonopt > g_optind) g_last_nonopt = g_optind;
    if (g_first_nonopt > g_optind) g_first_nonopt = g_optind;
    if (ordering == kPermute) {
      if (g_first_nonopt != g_last_nonopt && g_last_nonopt != g_optind) {
        Exchange(argv);
      } else if (g_last_nonopt != g_optind) {
        g_first_nonopt = g_optind;
      }
      while (g_optind < argc && IsOperand(argv, g_optind)) ++g_optind;
      g_last_nonopt = g_optind;
    }
    if (g_optind != argc && std::strcmp(argv[g_optind], "--") == 0) {
      ++g_optind;
      if (g_first_nonopt != g_last_nonopt && g_last_nonopt != g_optind) {
        Exchange(argv);
      } else if (g_first_nonopt == g_last_nonopt) {
        g_first_nonopt = g_optind;
      }
      g_last_nonopt = argc;
      g_optind = argc;
    }
    if (g_optind >= argc) {
      if (g_first_nonopt != g_last_nonopt) g_optind = g_first_nonopt;
      return -1;
    }
    if (IsOperand(argv, g_optind)) {
      if (ordering == kRequireOrder) return -1;
      g_optarg = argv[g_optind++];
      return 1;
    }
    // A long option: "--name", or "-name" for getopt_long_only.
    const char* arg = argv[g_optind];
    if (longopts != nullptr && (arg[1] == '-' || long_only)) {
      const char* name = arg + (arg[1] == '-' ? 2 : 1);
      const char* eq = std::strchr(name, '=');
      size_t len = eq != nullptr ? static_cast<size_t>(eq - name) : std::strlen(name);
      int found = -1, matches = 0;
      bool exact = false;
      for (int i = 0; longopts[i].name != nullptr; ++i) {
        if (std::strncmp(longopts[i].name, name, len) != 0) continue;
        if (std::strlen(longopts[i].name) == len) {
          found = i;
          exact = true;
          break;
        }
        if (found < 0) found = i;
        ++matches;
      }
      bool single_dash_short = arg[1] != '-' && std::strchr(optstring, name[0]) != nullptr && found < 0;
      if (!single_dash_short) {
        if (found >= 0 && !exact && matches > 1) {
          if (print) std::fprintf(stderr, "%s: option '%s' is ambiguous\n", prog, arg);
          ++g_optind;
          g_optopt = 0;
          return '?';
        }
        if (found < 0) {
          if (arg[1] == '-' || std::strchr(optstring, name[0]) == nullptr) {
            if (print) std::fprintf(stderr, "%s: unrecognized option '%s'\n", prog, arg);
            ++g_optind;
            g_optopt = 0;
            return '?';
          }
        } else {
          const BionicOption& o = longopts[found];
          ++g_optind;
          if (eq != nullptr) {
            if (o.has_arg == 0) {
              if (print) std::fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", prog, o.name);
              g_optopt = o.val;
              return '?';
            }
            g_optarg = const_cast<char*>(eq + 1);
          } else if (o.has_arg == 1) {  // required_argument
            if (g_optind < argc) {
              g_optarg = argv[g_optind++];
            } else {
              if (print) std::fprintf(stderr, "%s: option '--%s' requires an argument\n", prog, o.name);
              g_optopt = o.val;
              return colon ? ':' : '?';
            }
          }
          if (longindex != nullptr) *longindex = found;
          if (o.flag != nullptr) {
            *o.flag = o.val;
            return 0;
          }
          return o.val;
        }
      }
    }
    g_nextchar = arg + 1;
  }
  // A short option, possibly inside a cluster ("-abc").
  char c = *g_nextchar++;
  const char* spec = c != ':' ? std::strchr(optstring, c) : nullptr;
  if (*g_nextchar == '\0') ++g_optind;
  if (spec == nullptr) {
    if (print) std::fprintf(stderr, "%s: invalid option -- '%c'\n", prog, c);
    g_optopt = c;
    return '?';
  }
  if (spec[1] == ':') {
    if (spec[2] == ':') {  // an optional argument: only attached
      if (*g_nextchar != '\0') {
        g_optarg = const_cast<char*>(g_nextchar);
        ++g_optind;
      }
    } else if (*g_nextchar != '\0') {
      g_optarg = const_cast<char*>(g_nextchar);
      ++g_optind;
    } else if (g_optind < argc) {
      g_optarg = argv[g_optind++];
    } else {
      if (print) std::fprintf(stderr, "%s: option requires an argument -- '%c'\n", prog, c);
      g_optopt = c;
      g_nextchar = nullptr;
      return colon ? ':' : '?';
    }
    g_nextchar = nullptr;
  }
  return c;
}
int BIONIC_SYSV Getopt(int argc, char** argv, const char* optstring) {
  return GetoptImpl(argc, argv, optstring, nullptr, nullptr, false);
}
int BIONIC_SYSV GetoptLong(int argc, char** argv, const char* optstring, const BionicOption* longopts,
                           int* longindex) {
  return GetoptImpl(argc, argv, optstring, longopts, longindex, false);
}
int BIONIC_SYSV GetoptLongOnly(int argc, char** argv, const char* optstring,
                               const BionicOption* longopts, int* longindex) {
  return GetoptImpl(argc, argv, optstring, longopts, longindex, true);
}

// Temporary files: gesher-base's (Linux paths, O_* flags).
int BIONIC_SYSV Mkstemp(char* t) { return mkstemp(t); }
int BIONIC_SYSV Mkostemp(char* t, int flags) { return mkostemp(t, flags); }
int BIONIC_SYSV Mkstemps(char* t, int suffix) { return mkstemps(t, suffix); }
int BIONIC_SYSV Mkostemps(char* t, int suffix, int flags) {
  (void)flags;
  return mkstemps(t, suffix);
}
char* BIONIC_SYSV Mkdtemp(char* t) { return mkdtemp(t); }
char* BIONIC_SYSV Mktemp(char* t) {
  // The name only (insecure, as everywhere): the last six X's from a counter.
  static std::atomic<unsigned> n{0};
  size_t len = std::strlen(t);
  if (len < 6 || std::strcmp(t + len - 6, "XXXXXX") != 0) {
    errno = kEINVAL;
    if (len > 0) t[0] = 0;
    return t;
  }
  unsigned v = (GetCurrentProcessId() << 12) ^ n.fetch_add(1) ^ GetTickCount();
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int i = 0; i < 6; ++i, v /= 36) t[len - 6 + i] = kChars[v % 36];
  return t;
}

// Users and groups: the app's user, as bionic reports an app uid (u0_a<n>).
struct BionicPasswd {
  char* pw_name;
  char* pw_passwd;
  uint32_t pw_uid;
  uint32_t pw_gid;
  char* pw_gecos;
  char* pw_dir;
  char* pw_shell;
};
struct BionicGroup {
  char* gr_name;
  char* gr_passwd;
  uint32_t gr_gid;
  char** gr_mem;
};
thread_local BionicPasswd t_passwd;
thread_local BionicGroup t_group;
thread_local char t_pw_strings[256];
thread_local char* t_gr_mem[2];

std::string UserName(uint32_t uid) {
  if (uid == 0) return "root";
  if (uid == 1000) return "system";
  if (uid == 2000) return "shell";
  if (uid >= 10000 && uid < 20000) return "u0_a" + std::to_string(uid - 10000);
  return std::to_string(uid);
}
uint32_t UidOf(const char* name) {
  if (std::strcmp(name, "root") == 0) return 0;
  if (std::strcmp(name, "system") == 0) return 1000;
  if (std::strcmp(name, "shell") == 0) return 2000;
  if (std::strncmp(name, "u0_a", 4) == 0) return 10000 + static_cast<uint32_t>(std::atoi(name + 4));
  return static_cast<uint32_t>(-1);
}
// Fills `pw` with strings in `buf`; false if it is too small.
bool FillPasswd(uint32_t uid, BionicPasswd* pw, char* buf, size_t size) {
  std::string name = UserName(uid);
  const char* dir = "/data";
  const char* shell = "/system/bin/sh";
  size_t need = name.size() + 1 + std::strlen(dir) + 1 + std::strlen(shell) + 1 + 1;
  if (need > size) return false;
  char* p = buf;
  auto put = [&p](const char* s) {
    char* at = p;
    std::strcpy(p, s);
    p += std::strlen(s) + 1;
    return at;
  };
  pw->pw_name = put(name.c_str());
  pw->pw_passwd = nullptr;
  pw->pw_uid = uid;
  pw->pw_gid = uid;
  pw->pw_gecos = nullptr;
  pw->pw_dir = put(dir);
  pw->pw_shell = put(shell);
  return true;
}
BionicPasswd* BIONIC_SYSV Getpwuid(uint32_t uid) {
  if (uid == static_cast<uint32_t>(-1)) return errno = kENOENT, nullptr;
  return FillPasswd(uid, &t_passwd, t_pw_strings, sizeof(t_pw_strings)) ? &t_passwd : nullptr;
}
BionicPasswd* BIONIC_SYSV Getpwnam(const char* name) { return Getpwuid(UidOf(name)); }
int BIONIC_SYSV GetpwuidR(uint32_t uid, BionicPasswd* pw, char* buf, size_t size, BionicPasswd** out) {
  *out = nullptr;
  if (uid == static_cast<uint32_t>(-1)) return 0;
  if (!FillPasswd(uid, pw, buf, size)) return kERANGE;
  *out = pw;
  return 0;
}
int BIONIC_SYSV GetpwnamR(const char* name, BionicPasswd* pw, char* buf, size_t size,
                          BionicPasswd** out) {
  return GetpwuidR(UidOf(name), pw, buf, size, out);
}
BionicGroup* BIONIC_SYSV Getgrgid(uint32_t gid) {
  if (gid == static_cast<uint32_t>(-1)) return errno = kENOENT, nullptr;
  std::string name = UserName(gid);
  std::snprintf(t_pw_strings, sizeof(t_pw_strings), "%s", name.c_str());
  t_group.gr_name = t_pw_strings;
  t_group.gr_passwd = nullptr;
  t_group.gr_gid = gid;
  t_gr_mem[0] = t_pw_strings;
  t_gr_mem[1] = nullptr;
  t_group.gr_mem = t_gr_mem;
  return &t_group;
}
BionicGroup* BIONIC_SYSV Getgrnam(const char* name) { return Getgrgid(UidOf(name)); }
char* BIONIC_SYSV Getlogin() {
  static char name[64];
  std::snprintf(name, sizeof(name), "%s", UserName(static_cast<uint32_t>(getuid())).c_str());
  return name;
}
int BIONIC_SYSV GetloginR(char* buf, size_t size) {
  std::string name = UserName(static_cast<uint32_t>(getuid()));
  if (name.size() + 1 > size) return kERANGE;
  std::memcpy(buf, name.c_str(), name.size() + 1);
  return 0;
}
// No user database to walk (bionic's getpwent lists the Android ids).
void* BIONIC_SYSV Getpwent() { return nullptr; }
void BIONIC_SYSV Setpwent() {}
void BIONIC_SYSV Endpwent() {}

int BIONIC_SYSV Getloadavg(double* loads, int n) {
  for (int i = 0; i < n && i < 3; ++i) loads[i] = 0;
  return n < 3 ? n : 3;
}

// at_quick_exit/quick_exit: System V handlers, last registered first.
std::mutex g_quick_mutex;
std::vector<void (BIONIC_SYSV*)()> g_quick;
int BIONIC_SYSV AtQuickExit(void (BIONIC_SYSV* fn)()) {
  std::lock_guard<std::mutex> lock(g_quick_mutex);
  g_quick.push_back(fn);
  return 0;
}
[[noreturn]] void BIONIC_SYSV QuickExit(int code) {
  std::vector<void (BIONIC_SYSV*)()> fns;
  {
    std::lock_guard<std::mutex> lock(g_quick_mutex);
    fns.swap(g_quick);
  }
  for (auto it = fns.rbegin(); it != fns.rend(); ++it) (*it)();
  std::fflush(nullptr);
  ExitProcess(static_cast<UINT>(code));
}

// --- strings ------------------------------------------------------------------

void* BIONIC_SYSV Memccpy(void* d, const void* s, int c, size_t n) {
  auto dp = static_cast<uint8_t*>(d);
  auto sp = static_cast<const uint8_t*>(s);
  for (size_t i = 0; i < n; ++i) {
    dp[i] = sp[i];
    if (sp[i] == static_cast<uint8_t>(c)) return dp + i + 1;
  }
  return nullptr;
}
void* BIONIC_SYSV Mempcpy(void* d, const void* s, size_t n) {
  std::memcpy(d, s, n);
  return static_cast<uint8_t*>(d) + n;
}
void BIONIC_SYSV Bzero(void* d, size_t n) { std::memset(d, 0, n); }
void BIONIC_SYSV ExplicitBzero(void* d, size_t n) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(d);
  while (n-- > 0) *p++ = 0;
}
int BIONIC_SYSV Bcmp(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) != 0; }
void BIONIC_SYSV Bcopy(const void* s, void* d, size_t n) { std::memmove(d, s, n); }
int BIONIC_SYSV Ffs(int v) { return v == 0 ? 0 : __builtin_ctz(static_cast<unsigned>(v)) + 1; }
int BIONIC_SYSV Ffsl(int64_t v) { return v == 0 ? 0 : __builtin_ctzll(static_cast<uint64_t>(v)) + 1; }
void BIONIC_SYSV Swab(const void* s, void* d, ssize_t n) {
  auto sp = static_cast<const uint8_t*>(s);
  auto dp = static_cast<uint8_t*>(d);
  for (ssize_t i = 0; i + 1 < n; i += 2) {
    uint8_t a = sp[i], b = sp[i + 1];
    dp[i] = b;
    dp[i + 1] = a;
  }
}
size_t BIONIC_SYSV Strxfrm(char* d, const char* s, size_t n) {
  size_t len = std::strlen(s);
  if (n > len) std::memcpy(d, s, len + 1);
  return len;
}
// GNU strverscmp: digit runs compare by value (with leading zeros as fractions).
int BIONIC_SYSV Strverscmp(const char* a, const char* b) {
  while (*a && *a == *b && !std::isdigit(static_cast<unsigned char>(*a))) ++a, ++b;
  if (!std::isdigit(static_cast<unsigned char>(*a)) || !std::isdigit(static_cast<unsigned char>(*b))) {
    // Back up into a digit run both share, if any.
    return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
  }
  bool fa = *a == '0', fb = *b == '0';
  if (fa || fb) {  // fractional parts: compare as strings, the longer run first loses
    while (*a == *b && std::isdigit(static_cast<unsigned char>(*a))) ++a, ++b;
    if (std::isdigit(static_cast<unsigned char>(*a)) && std::isdigit(static_cast<unsigned char>(*b)))
      return static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
    if (std::isdigit(static_cast<unsigned char>(*a))) return 1;
    if (std::isdigit(static_cast<unsigned char>(*b))) return -1;
    return Strverscmp(a, b);
  }
  const char *ea = a, *eb = b;
  while (std::isdigit(static_cast<unsigned char>(*ea))) ++ea;
  while (std::isdigit(static_cast<unsigned char>(*eb))) ++eb;
  if (ea - a != eb - b) return (ea - a) < (eb - b) ? -1 : 1;
  int c = std::strncmp(a, b, static_cast<size_t>(ea - a));
  if (c != 0) return c;
  return Strverscmp(ea, eb);
}
// The *_l forms: one locale (C/UTF-8), as locale_bionic.cc.
const char* BIONIC_SYSV StrerrorL(int e, void*) { return strerror(e); }
int BIONIC_SYSV StrcasecmpL(const char* a, const char* b, void*) { return _stricmp(a, b); }
int BIONIC_SYSV StrncasecmpL(const char* a, const char* b, size_t n, void*) { return _strnicmp(a, b, n); }
int BIONIC_SYSV ToupperL(int c, void*) { return std::toupper(c); }
int BIONIC_SYSV TolowerL(int c, void*) { return std::tolower(c); }
#define CTYPE_L(name) \
  int BIONIC_SYSV name##_L(int c, void*) { return std::name(c); }
CTYPE_L(isalpha) CTYPE_L(isdigit) CTYPE_L(isalnum) CTYPE_L(isspace) CTYPE_L(isupper)
CTYPE_L(islower) CTYPE_L(isxdigit) CTYPE_L(isprint) CTYPE_L(ispunct) CTYPE_L(iscntrl)
CTYPE_L(isgraph)
#undef CTYPE_L
int BIONIC_SYSV Isblank(int c) { return c == ' ' || c == '\t'; }
int BIONIC_SYSV IsblankL(int c, void*) { return Isblank(c); }
int BIONIC_SYSV Isascii(int c) { return (c & ~0x7f) == 0; }
int BIONIC_SYSV Toascii(int c) { return c & 0x7f; }
double BIONIC_SYSV StrtodL(const char* s, char** e, void*) { return std::strtod(s, e); }
float BIONIC_SYSV StrtofL(const char* s, char** e, void*) { return std::strtof(s, e); }
int64_t BIONIC_SYSV StrtolL(const char* s, char** e, int base, void*) { return std::strtoll(s, e, base); }
uint64_t BIONIC_SYSV StrtoulL(const char* s, char** e, int base, void*) { return std::strtoull(s, e, base); }

// --- wide characters (bionic's wchar_t is 32-bit) ----------------------------------

#define WCLASS(name, property)                                                \
  int BIONIC_SYSV W_##name(uint32_t c) {                                       \
    return UnicodeValue(c, UnicodeProperty::property);                        \
  }
WCLASS(iswalpha, Alpha) WCLASS(iswalnum, Alnum) WCLASS(iswdigit, Digit)
WCLASS(iswxdigit, Xdigit) WCLASS(iswspace, Space) WCLASS(iswupper, Upper)
WCLASS(iswlower, Lower) WCLASS(iswcntrl, Cntrl) WCLASS(iswpunct, Punct)
WCLASS(iswprint, Print) WCLASS(iswgraph, Graph) WCLASS(iswblank, Blank)
#undef WCLASS
uint32_t BIONIC_SYSV W_towlower(uint32_t c) {
  return UnicodeValue(c, UnicodeProperty::ToLower);
}
uint32_t BIONIC_SYSV W_towupper(uint32_t c) {
  return UnicodeValue(c, UnicodeProperty::ToUpper);
}
// wctype/iswctype and wctrans/towctrans: small integer handles.
const char* const kClasses[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                "lower", "print", "punct", "space", "upper", "xdigit"};
uint64_t BIONIC_SYSV Wctype(const char* name) {
  for (uint64_t i = 0; i < 12; ++i) {
    if (std::strcmp(name, kClasses[i]) == 0) return i + 1;
  }
  return 0;
}
int BIONIC_SYSV Iswctype(uint32_t c, uint64_t t) {
  switch (t) {
    case 1: return W_iswalnum(c);
    case 2: return W_iswalpha(c);
    case 3: return W_iswblank(c);
    case 4: return W_iswcntrl(c);
    case 5: return W_iswdigit(c);
    case 6: return W_iswgraph(c);
    case 7: return W_iswlower(c);
    case 8: return W_iswprint(c);
    case 9: return W_iswpunct(c);
    case 10: return W_iswspace(c);
    case 11: return W_iswupper(c);
    case 12: return W_iswxdigit(c);
    default: return 0;
  }
}
int BIONIC_SYSV IswctypeL(uint32_t c, uint64_t t, void*) { return Iswctype(c, t); }
uint64_t BIONIC_SYSV WctypeL(const char* name, void*) { return Wctype(name); }
const void* BIONIC_SYSV Wctrans(const char* name) {
  if (std::strcmp(name, "tolower") == 0) return reinterpret_cast<const void*>(1);
  if (std::strcmp(name, "toupper") == 0) return reinterpret_cast<const void*>(2);
  errno = EINVAL;
  return nullptr;
}
uint32_t BIONIC_SYSV Towctrans(uint32_t c, const void* t) {
  if (t == reinterpret_cast<const void*>(1)) return W_towlower(c);
  if (t == reinterpret_cast<const void*>(2)) return W_towupper(c);
  errno = EINVAL;
  return c;
}
const void* BIONIC_SYSV WctransL(const char* name, void*) { return Wctrans(name); }
uint32_t BIONIC_SYSV TowctransL(uint32_t c, const void* t, void*) { return Towctrans(c, t); }
int BIONIC_SYSV Wcwidth(uint32_t c) { return UnicodeWidth(c); }
int BIONIC_SYSV Wcswidth(const wchar32* s, size_t n) {
  int total = 0;
  for (size_t i = 0; i < n && s[i] != 0; ++i) {
    int w = Wcwidth(s[i]);
    if (w < 0) return -1;
    total += w;
  }
  return total;
}

size_t Wlen(const wchar32* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
int BIONIC_SYSV Wcscmp(const wchar32* a, const wchar32* b) {
  while (*a && *a == *b) ++a, ++b;
  return static_cast<int32_t>(*a - *b);
}
int BIONIC_SYSV Wcsncmp(const wchar32* a, const wchar32* b, size_t n) {
  for (; n > 0; --n, ++a, ++b) {
    if (*a != *b) return static_cast<int32_t>(*a - *b);
    if (*a == 0) break;
  }
  return 0;
}
int BIONIC_SYSV Wcsncasecmp(const wchar32* a, const wchar32* b, size_t n) {
  for (; n > 0; --n, ++a, ++b) {
    wchar32 x = W_towlower(*a), y = W_towlower(*b);
    if (x != y) return x < y ? -1 : 1;
    if (x == 0) break;
  }
  return 0;
}
int BIONIC_SYSV Wcscasecmp(const wchar32* a, const wchar32* b) { return Wcsncasecmp(a, b, SIZE_MAX); }
int BIONIC_SYSV WcscasecmpL(const wchar32* a, const wchar32* b, void*) { return Wcscasecmp(a, b); }
int BIONIC_SYSV WcsncasecmpL(const wchar32* a, const wchar32* b, size_t n, void*) {
  return Wcsncasecmp(a, b, n);
}
wchar32* BIONIC_SYSV Wcschr(const wchar32* s, wchar32 c) {
  for (;; ++s) {
    if (*s == c) return const_cast<wchar32*>(s);
    if (*s == 0) return nullptr;
  }
}
wchar32* BIONIC_SYSV Wcsrchr(const wchar32* s, wchar32 c) {
  const wchar32* last = nullptr;
  for (;; ++s) {
    if (*s == c) last = s;
    if (*s == 0) return const_cast<wchar32*>(last);
  }
}
wchar32* BIONIC_SYSV Wcsstr(const wchar32* h, const wchar32* n) {
  size_t nl = Wlen(n);
  if (nl == 0) return const_cast<wchar32*>(h);
  for (; *h; ++h) {
    if (Wcsncmp(h, n, nl) == 0) return const_cast<wchar32*>(h);
  }
  return nullptr;
}
wchar32* BIONIC_SYSV Wcscpy(wchar32* d, const wchar32* s) {
  std::memcpy(d, s, (Wlen(s) + 1) * 4);
  return d;
}
wchar32* BIONIC_SYSV Wcpcpy(wchar32* d, const wchar32* s) {
  size_t n = Wlen(s);
  std::memcpy(d, s, (n + 1) * 4);
  return d + n;
}
wchar32* BIONIC_SYSV Wcsncpy(wchar32* d, const wchar32* s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  for (; i < n; ++i) d[i] = 0;
  return d;
}
wchar32* BIONIC_SYSV Wcpncpy(wchar32* d, const wchar32* s, size_t n) {
  size_t i = 0;
  for (; i < n && s[i]; ++i) d[i] = s[i];
  wchar32* end = d + i;
  for (; i < n; ++i) d[i] = 0;
  return end;
}
wchar32* BIONIC_SYSV Wcscat(wchar32* d, const wchar32* s) {
  Wcscpy(d + Wlen(d), s);
  return d;
}
wchar32* BIONIC_SYSV Wcsncat(wchar32* d, const wchar32* s, size_t n) {
  wchar32* p = d + Wlen(d);
  size_t i = 0;
  for (; i < n && s[i]; ++i) p[i] = s[i];
  p[i] = 0;
  return d;
}
size_t BIONIC_SYSV Wcslcpy(wchar32* d, const wchar32* s, size_t n) {
  size_t len = Wlen(s);
  if (n > 0) {
    size_t c = len < n - 1 ? len : n - 1;
    std::memcpy(d, s, c * 4);
    d[c] = 0;
  }
  return len;
}
size_t BIONIC_SYSV Wcslcat(wchar32* d, const wchar32* s, size_t n) {
  size_t dl = 0;
  while (dl < n && d[dl]) ++dl;
  if (dl == n) return n + Wlen(s);
  return dl + Wcslcpy(d + dl, s, n - dl);
}
size_t BIONIC_SYSV Wcsnlen(const wchar32* s, size_t n) {
  size_t i = 0;
  while (i < n && s[i]) ++i;
  return i;
}
wchar32* BIONIC_SYSV Wcsdup(const wchar32* s) {
  size_t n = (Wlen(s) + 1) * 4;
  auto p = static_cast<wchar32*>(std::malloc(n));
  if (p) std::memcpy(p, s, n);
  return p;
}
bool InSet(wchar32 c, const wchar32* set) {
  for (; *set; ++set) {
    if (*set == c) return true;
  }
  return false;
}
size_t BIONIC_SYSV Wcsspn(const wchar32* s, const wchar32* set) {
  size_t i = 0;
  while (s[i] && InSet(s[i], set)) ++i;
  return i;
}
size_t BIONIC_SYSV Wcscspn(const wchar32* s, const wchar32* set) {
  size_t i = 0;
  while (s[i] && !InSet(s[i], set)) ++i;
  return i;
}
wchar32* BIONIC_SYSV Wcspbrk(const wchar32* s, const wchar32* set) {
  for (; *s; ++s) {
    if (InSet(*s, set)) return const_cast<wchar32*>(s);
  }
  return nullptr;
}
wchar32* BIONIC_SYSV Wcstok(wchar32* s, const wchar32* delim, wchar32** save) {
  if (s == nullptr) s = *save;
  if (s == nullptr) return nullptr;
  s += Wcsspn(s, delim);
  if (*s == 0) {
    *save = nullptr;
    return nullptr;
  }
  wchar32* end = s + Wcscspn(s, delim);
  if (*end) {
    *end = 0;
    *save = end + 1;
  } else {
    *save = nullptr;
  }
  return s;
}
wchar32* BIONIC_SYSV Wmemcpy(wchar32* d, const wchar32* s, size_t n) {
  std::memcpy(d, s, n * 4);
  return d;
}
wchar32* BIONIC_SYSV Wmempcpy(wchar32* d, const wchar32* s, size_t n) {
  std::memcpy(d, s, n * 4);
  return d + n;
}
wchar32* BIONIC_SYSV Wmemmove(wchar32* d, const wchar32* s, size_t n) {
  std::memmove(d, s, n * 4);
  return d;
}
wchar32* BIONIC_SYSV Wmemset(wchar32* d, wchar32 c, size_t n) {
  for (size_t i = 0; i < n; ++i) d[i] = c;
  return d;
}
// UTF-8 <-> UTF-32 for the multibyte conversions below.
std::string Utf8(const wchar32* s, size_t n = SIZE_MAX) {
  std::string out;
  for (size_t i = 0; i < n && s[i] != 0; ++i) {
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
}  // namespace

// Wide formatted output: the narrow formatter (stdio_bionic.cc) on the
// format converted to UTF-8, which handles %ls and %lc itself.
extern "C" int BIONIC_SYSV gesher_extra_vfwprintf(void* f, const wchar32* fmt, SysvVaList* ap) {
  std::string narrow = Utf8(fmt);
  return gesher_bionic_vfprintf(f, narrow.c_str(), ap);
}
extern "C" int BIONIC_SYSV gesher_extra_vwprintf(const wchar32* fmt, SysvVaList* ap) {
  static void* out = LookupBionicSymbol("stdout");
  return gesher_extra_vfwprintf(out != nullptr ? *static_cast<void**>(out) : stdout, fmt, ap);
}
namespace {

wchar32* BIONIC_SYSV Fgetws(wchar32* buf, int n, void* f) {
  static auto fgetwc = Shim<uint32_t(BIONIC_SYSV*)(void*)>("fgetwc");
  if (n <= 0 || fgetwc == nullptr) return nullptr;
  int i = 0;
  while (i < n - 1) {
    uint32_t c = fgetwc(f);
    if (c == 0xffffffffu) break;  // WEOF
    buf[i++] = c;
    if (c == '\n') break;
  }
  if (i == 0) return nullptr;
  buf[i] = 0;
  return buf;
}

// --- stdio: unlocked forms, buffers, memory streams -----------------------------
// The .so's FILE* is the CRT's (stdio_bionic.cc), or &__sF[n] for the three
// standard streams in libraries built for API < 23.
FILE* F(void* f) {
  static auto sf = static_cast<uint8_t*>(LookupBionicSymbol("__sF"));
  auto p = static_cast<uint8_t*>(f);
  if (sf != nullptr && p >= sf && p < sf + 3 * 152) {
    switch ((p - sf) / 152) {
      case 0: return stdin;
      case 1: return stdout;
      default: return stderr;
    }
  }
  return static_cast<FILE*>(f);
}
void BIONIC_SYSV Flockfile(void* f) { _lock_file(F(f)); }
void BIONIC_SYSV Funlockfile(void* f) { _unlock_file(F(f)); }
int BIONIC_SYSV Ftrylockfile(void* f) {
  _lock_file(F(f));  // the CRT has no try form
  return 0;
}
int BIONIC_SYSV GetcUnlocked(void* f) { return _getc_nolock(F(f)); }
int BIONIC_SYSV GetcharUnlocked() { return _getc_nolock(stdin); }
int BIONIC_SYSV PutcUnlocked(int c, void* f) { return _putc_nolock(c, F(f)); }
int BIONIC_SYSV PutcharUnlocked(int c) { return _putc_nolock(c, stdout); }
size_t BIONIC_SYSV FreadUnlocked(void* b, size_t s, size_t n, void* f) { return _fread_nolock(b, s, n, F(f)); }
size_t BIONIC_SYSV FwriteUnlocked(const void* b, size_t s, size_t n, void* f) {
  return _fwrite_nolock(b, s, n, F(f));
}
int BIONIC_SYSV FputsUnlocked(const char* s, void* f) { return std::fputs(s, F(f)); }
char* BIONIC_SYSV FgetsUnlocked(char* s, int n, void* f) { return std::fgets(s, n, F(f)); }
int BIONIC_SYSV FflushUnlocked(void* f) { return _fflush_nolock(f == nullptr ? nullptr : F(f)); }
int BIONIC_SYSV FeofUnlocked(void* f) { return std::feof(F(f)); }
int BIONIC_SYSV FerrorUnlocked(void* f) { return std::ferror(F(f)); }
void BIONIC_SYSV ClearerrUnlocked(void* f) { std::clearerr(F(f)); }
int BIONIC_SYSV FilenoUnlocked(void* f) { return _fileno(F(f)); }
int BIONIC_SYSV Fsetlocking(void*, int) { return 1; }  // FSETLOCKING_INTERNAL
void BIONIC_SYSV Setlinebuf(void* f) { std::setvbuf(F(f), nullptr, _IOLBF, BUFSIZ); }
void BIONIC_SYSV Setbuffer(void* f, char* buf, size_t size) {
  std::setvbuf(F(f), buf, buf != nullptr ? _IOFBF : _IONBF, size);
}
int BIONIC_SYSV Getw(void* f) {
  int v;
  return std::fread(&v, sizeof(v), 1, F(f)) == 1 ? v : EOF;
}
int BIONIC_SYSV Putw(int v, void* f) { return std::fwrite(&v, sizeof(v), 1, F(f)) == 1 ? 0 : EOF; }
// fmemopen: a temporary file holding the buffer's bytes; reads see them, and
// writes reach the buffer when the stream is flushed or closed only through
// the open_memstream path below, so this is for reading (the common use).
void* BIONIC_SYSV Fmemopen(void* buf, size_t size, const char* mode) {
  static auto tmpfile_fn = Shim<FILE*(BIONIC_SYSV*)()>("tmpfile");
  FILE* f = tmpfile_fn != nullptr ? tmpfile_fn() : std::tmpfile();
  if (f == nullptr) return nullptr;
  if (buf != nullptr && mode != nullptr && (mode[0] == 'r' || std::strchr(mode, '+') != nullptr)) {
    size_t n = size;
    if (mode[0] != 'r') n = strnlen(static_cast<const char*>(buf), size);
    if (n > 0 && std::fwrite(buf, 1, n, f) != n) {
      std::fclose(f);
      return nullptr;
    }
    std::fflush(f);
    if (mode[0] == 'a') {
      std::fseek(f, 0, SEEK_END);
    } else {
      std::rewind(f);
    }
  }
  return f;
}
char* BIONIC_SYSV Ctermid(char* s) {
  static char tty[] = "/dev/tty";
  if (s == nullptr) return tty;
  std::strcpy(s, tty);
  return s;
}
char* BIONIC_SYSV Tmpnam(char* s) {
  static char name[64];
  static std::atomic<unsigned> n{0};
  char* out = s != nullptr ? s : name;
  const char* dir = std::getenv("TMPDIR");
  std::snprintf(out, 64, "%s/tmp.%lu.%u", dir != nullptr ? dir : "/data/local/tmp",
                static_cast<unsigned long>(GetCurrentProcessId()), n.fetch_add(1));
  return out;
}
char* BIONIC_SYSV Tempnam(const char* dir, const char* prefix) {
  static std::atomic<unsigned> n{0};
  const char* d = dir != nullptr ? dir : std::getenv("TMPDIR");
  std::string s = std::string(d != nullptr ? d : "/data/local/tmp") + "/" + (prefix ? prefix : "tmp") + "." +
                  std::to_string(GetCurrentProcessId()) + "." + std::to_string(n.fetch_add(1));
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p) std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

// --- strptime (the C locale) --------------------------------------------------------
// bionic's struct tm: nine ints, then long tm_gmtoff and const char* tm_zone.
struct BionicTm {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  int64_t tm_gmtoff;
  const char* tm_zone;
};
const char* const kDays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
const char* const kMonths[] = {"january", "february", "march", "april", "may", "june", "july",
                               "august", "september", "october", "november", "december"};
bool Number(const char** s, int min, int max, int digits, int* out) {
  while (**s == ' ') ++*s;
  bool neg = false;
  if (min < 0 && (**s == '-' || **s == '+')) {
    neg = **s == '-';
    ++*s;
  }
  int v = 0, n = 0;
  while (n < digits && std::isdigit(static_cast<unsigned char>(**s))) {
    v = v * 10 + (**s - '0');
    ++*s;
    ++n;
  }
  if (n == 0) return false;
  if (neg) v = -v;
  if (v < min || v > max) return false;
  *out = v;
  return true;
}
bool Name(const char** s, const char* const* names, int count, int* out) {
  while (**s == ' ') ++*s;
  for (int i = 0; i < count; ++i) {
    size_t full = std::strlen(names[i]);
    if (_strnicmp(*s, names[i], full) == 0) {
      *s += full;
      *out = i;
      return true;
    }
    if (_strnicmp(*s, names[i], 3) == 0) {
      *s += 3;
      *out = i;
      return true;
    }
  }
  return false;
}
int DaysBefore(int year, int mon) {
  static const int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return kCum[mon] + (leap && mon > 1 ? 1 : 0);
}
const char* StrptimeImpl(const char* s, const char* fmt, BionicTm* tm, bool* have_date) {
  int century = -1, yy = -1, pm = -1;
  while (*fmt) {
    if (std::isspace(static_cast<unsigned char>(*fmt))) {
      while (std::isspace(static_cast<unsigned char>(*s))) ++s;
      ++fmt;
      continue;
    }
    if (*fmt != '%') {
      if (*s++ != *fmt++) return nullptr;
      continue;
    }
    ++fmt;
    if (*fmt == 'E' || *fmt == 'O') ++fmt;
    int v;
    switch (*fmt++) {
      case '%':
        if (*s++ != '%') return nullptr;
        break;
      case 'a': case 'A':
        if (!Name(&s, kDays, 7, &v)) return nullptr;
        tm->tm_wday = v;
        break;
      case 'b': case 'B': case 'h':
        if (!Name(&s, kMonths, 12, &v)) return nullptr;
        tm->tm_mon = v;
        *have_date = true;
        break;
      case 'C':
        if (!Number(&s, 0, 99, 2, &century)) return nullptr;
        break;
      case 'd': case 'e':
        if (!Number(&s, 1, 31, 2, &tm->tm_mday)) return nullptr;
        *have_date = true;
        break;
      case 'D':
        s = StrptimeImpl(s, "%m/%d/%y", tm, have_date);
        if (s == nullptr) return nullptr;
        break;
      case 'F':
        s = StrptimeImpl(s, "%Y-%m-%d", tm, have_date);
        if (s == nullptr) return nullptr;
        break;
      case 'H': case 'k':
        if (!Number(&s, 0, 23, 2, &tm->tm_hour)) return nullptr;
        break;
      case 'I': case 'l':
        if (!Number(&s, 1, 12, 2, &tm->tm_hour)) return nullptr;
        break;
      case 'j':
        if (!Number(&s, 1, 366, 3, &v)) return nullptr;
        tm->tm_yday = v - 1;
        break;
      case 'm':
        if (!Number(&s, 1, 12, 2, &v)) return nullptr;
        tm->tm_mon = v - 1;
        *have_date = true;
        break;
      case 'M':
        if (!Number(&s, 0, 59, 2, &tm->tm_min)) return nullptr;
        break;
      case 'n': case 't':
        while (std::isspace(static_cast<unsigned char>(*s))) ++s;
        break;
      case 'p': case 'P':
        while (*s == ' ') ++s;
        if (_strnicmp(s, "am", 2) == 0) {
          pm = 0;
        } else if (_strnicmp(s, "pm", 2) == 0) {
          pm = 1;
        } else {
          return nullptr;
        }
        s += 2;
        break;
      case 'r':
        s = StrptimeImpl(s, "%I:%M:%S %p", tm, have_date);
        if (s == nullptr) return nullptr;
        break;
      case 'R':
        s = StrptimeImpl(s, "%H:%M", tm, have_date);
        if (s == nullptr) return nullptr;
        break;
      case 's': {
        while (*s == ' ') ++s;
        char* end;
        long long t = std::strtoll(s, &end, 10);
        if (end == s) return nullptr;
        s = end;
        time_t tt = static_cast<time_t>(t);
        struct tm g;
        if (gmtime_s(&g, &tt) != 0) return nullptr;
        tm->tm_sec = g.tm_sec, tm->tm_min = g.tm_min, tm->tm_hour = g.tm_hour;
        tm->tm_mday = g.tm_mday, tm->tm_mon = g.tm_mon, tm->tm_year = g.tm_year;
        tm->tm_wday = g.tm_wday, tm->tm_yday = g.tm_yday;
        break;
      }
      case 'S':
        if (!Number(&s, 0, 61, 2, &tm->tm_sec)) return nullptr;
        break;
      case 'T':
        s = StrptimeImpl(s, "%H:%M:%S", tm, have_date);
        if (s == nullptr) return nullptr;
        break;
      case 'u':
        if (!Number(&s, 1, 7, 1, &v)) return nullptr;
        tm->tm_wday = v % 7;
        break;
      case 'w':
        if (!Number(&s, 0, 6, 1, &tm->tm_wday)) return nullptr;
        break;
      case 'y':
        if (!Number(&s, 0, 99, 2, &yy)) return nullptr;
        *have_date = true;
        break;
      case 'Y':
        if (!Number(&s, -9999, 99999, 5, &v)) return nullptr;
        tm->tm_year = v - 1900;
        *have_date = true;
        break;
      case 'z': {
        while (*s == ' ') ++s;
        if (*s == 'Z') {
          ++s;
          tm->tm_gmtoff = 0;
          break;
        }
        if (*s != '+' && *s != '-') return nullptr;
        int sign = *s++ == '-' ? -1 : 1;
        int hh, mm = 0;
        if (!Number(&s, 0, 99, 2, &hh)) return nullptr;
        if (*s == ':') ++s;
        Number(&s, 0, 59, 2, &mm);
        tm->tm_gmtoff = sign * (hh * 3600 + mm * 60);
        break;
      }
      case 'Z':
        while (std::isalpha(static_cast<unsigned char>(*s))) ++s;
        break;
      default:
        return nullptr;
    }
  }
  if (yy >= 0) {
    tm->tm_year = century >= 0 ? century * 100 + yy - 1900 : (yy < 69 ? yy + 100 : yy);
  } else if (century >= 0) {
    tm->tm_year = century * 100 - 1900;
  }
  if (pm >= 0) {
    tm->tm_hour %= 12;
    if (pm) tm->tm_hour += 12;
  }
  return s;
}
char* BIONIC_SYSV Strptime(const char* s, const char* fmt, BionicTm* tm) {
  bool have_date = false;
  const char* end = StrptimeImpl(s, fmt, tm, &have_date);
  if (end != nullptr && have_date && tm->tm_mon >= 0 && tm->tm_mon < 12) {
    // Like glibc: the day of the year and of the week from the date.
    int year = tm->tm_year + 1900;
    tm->tm_yday = DaysBefore(year, tm->tm_mon) + tm->tm_mday - 1;
    int64_t y = year - 1;
    int64_t days = y * 365 + y / 4 - y / 100 + y / 400 + tm->tm_yday;  // since 0001-01-01 (a Monday)
    tm->tm_wday = static_cast<int>((days + 1) % 7);
  }
  return const_cast<char*>(end);
}
char* BIONIC_SYSV StrptimeL(const char* s, const char* fmt, BionicTm* tm, void*) {
  return Strptime(s, fmt, tm);
}

// --- system properties --------------------------------------------------------------

std::mutex g_prop_mutex;
PropertyGetter g_prop_get = nullptr;
PropertySetter g_prop_set = nullptr;

struct PropInfo {
  std::string name;
  std::string immutable_value;
  uint32_t immutable_serial = 0;
};
std::map<std::string, std::unique_ptr<PropInfo>>* g_prop_infos = nullptr;

bool GetProp(const char* name, std::string* value) {
  if (name == nullptr) return false;
  PropertyGetter get;
  {
    std::lock_guard<std::mutex> lock(g_prop_mutex);
    get = g_prop_get;
  }
  if (get != nullptr && get(name, value)) return true;
  return properties::Get(name, value);
}

int BIONIC_SYSV SystemPropertyGet(const char* name, char* value) { return SystemPropertyGetValue(name, value); }
int BIONIC_SYSV SystemPropertySet(const char* name, const char* value) {
  if (name == nullptr) return -1;
  if (value == nullptr) value = "";
  PropertySetter set;
  {
    std::lock_guard<std::mutex> lock(g_prop_mutex);
    set = g_prop_set;
  }
  return (set != nullptr ? set(name, value) : properties::Set(name, value)) ? 0 : -1;
}
PropInfo* Info(const std::string& name) {
  std::string value;
  uint32_t serial = 0;
  properties::Get(name, &value, &serial);
  std::lock_guard<std::mutex> lock(g_prop_mutex);
  if (g_prop_infos == nullptr) g_prop_infos = new std::map<std::string, std::unique_ptr<PropInfo>>();
  auto& slot = (*g_prop_infos)[name];
  if (!slot) {
    slot = std::make_unique<PropInfo>();
    slot->name = name;
    slot->immutable_value = std::move(value);
    slot->immutable_serial = serial;
  }
  return slot.get();
}
const PropInfo* BIONIC_SYSV SystemPropertyFind(const char* name) {
  std::string v;
  if (!GetProp(name, &v)) return nullptr;
  return Info(name);
}
int BIONIC_SYSV SystemPropertyRead(const PropInfo* pi, char* name, char* value) {
  if (pi == nullptr) return -1;
  if (name != nullptr) {
    size_t n = pi->name.size() < 31 ? pi->name.size() : 31;  // PROP_NAME_MAX
    std::memcpy(name, pi->name.data(), n);
    name[n] = 0;
  }
  return SystemPropertyGetValue(pi->name.c_str(), value != nullptr ? value : nullptr);
}
void BIONIC_SYSV SystemPropertyReadCallback(const PropInfo* pi,
                                            void (BIONIC_SYSV* cb)(void*, const char*, const char*, uint32_t),
                                            void* cookie) {
  if (pi == nullptr || cb == nullptr) return;
  if (pi->name.rfind("ro.", 0) == 0) {
    cb(cookie, pi->name.c_str(), pi->immutable_value.c_str(), pi->immutable_serial);
    return;
  }
  std::string v;
  uint32_t serial = 0;
  properties::Get(pi->name, &v, &serial);
  cb(cookie, pi->name.c_str(), v.c_str(), serial);
}
int BIONIC_SYSV SystemPropertyForeach(void (BIONIC_SYSV* cb)(const PropInfo*, void*), void* cookie) {
  if (cb == nullptr) return -1;
  std::vector<std::string> names;
  for (const auto& [name, value] : properties::List()) names.push_back(name);
  for (const std::string& n : names) cb(Info(n), cookie);
  return 0;
}
uint32_t BIONIC_SYSV SystemPropertySerial(const PropInfo* pi) {
  uint32_t serial = 0;
  if (pi) properties::Get(pi->name, nullptr, &serial);
  return serial;
}
uint32_t BIONIC_SYSV SystemPropertyAreaSerial() { return properties::AreaSerial(); }
bool BIONIC_SYSV SystemPropertyWait(const PropInfo* pi, uint32_t old_serial, uint32_t* new_serial,
                                  const LinuxTimespec* timeout) {
  properties::WaitTimeout relative{};
  if (timeout) relative = {timeout->tv_sec, timeout->tv_nsec};
  return properties::Wait(pi ? &pi->name : nullptr, old_serial, new_serial, timeout ? &relative : nullptr);
}
uint32_t BIONIC_SYSV SystemPropertyWaitAny(uint32_t old_serial) {
  uint32_t serial = old_serial;
  properties::Wait(nullptr, old_serial, &serial, nullptr);
  return serial;
}
const PropInfo* BIONIC_SYSV SystemPropertyFindNth(unsigned n) {
  std::string name;
  auto props = properties::List();
  if (n >= props.size()) return nullptr;
  auto it = props.begin();
  std::advance(it, n);
  name = it->first;
  return Info(name);
}
// Android's device API level and the application's target SDK are distinct.
int BIONIC_SYSV GetDeviceApiLevel() {
  std::string value;
  return GetProp("ro.build.version.sdk", &value) ? std::atoi(value.c_str()) : -1;
}
// android_get_application_target_sdk_version and friends (libc, API 24).
int g_target_sdk = 36;
int BIONIC_SYSV GetTargetSdk() { return g_target_sdk; }
void BIONIC_SYSV SetTargetSdk(int v) { g_target_sdk = v; }
void BIONIC_SYSV GetLdLibraryPath(char* buf, size_t size) {
  std::string all;
  for (const std::string& d : LibraryDirectories()) all += (all.empty() ? "" : ":") + d;
  if (size > 0) {
    size_t c = all.size() < size - 1 ? all.size() : size - 1;
    std::memcpy(buf, all.data(), c);
    buf[c] = 0;
  }
}
void BIONIC_SYSV UpdateLdLibraryPath(const char* path) {
  if (path == nullptr) return;
  std::string p = path;
  size_t start = 0;
  while (start <= p.size()) {
    size_t colon = p.find(':', start);
    std::string d = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!d.empty()) NoteLibraryDirectory(d);
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
}

// --- platform libraries for dlopen -------------------------------------------------

struct PlatformLib {
  const char* soname;
  const char* probe;  // a symbol the shim has when it implements the library
};
const PlatformLib kPlatformLibs[] = {
    {"libc.so", "malloc"},
    {"libm.so", "sin"},
    {"libdl.so", "dlopen"},
    {"liblog.so", "__android_log_print"},
    {"libz.so", "inflate"},
    {"libandroid.so", "AAssetManager_open"},
    {"libnativewindow.so", "ANativeWindow_acquire"},
    {"libEGL.so", "eglGetDisplay"},
    {"libGLESv1_CM.so", "glClear"},
    {"libGLESv2.so", "glClear"},
    {"libGLESv3.so", "glClear"},
    {"libjnigraphics.so", "AndroidBitmap_getInfo"},
    {"libOpenSLES.so", "slCreateEngine"},
    {"libaaudio.so", "AAudio_createStreamBuilder"},
    {"libmediandk.so", "AMediaFormat_new"},
    {"libcamera2ndk.so", "ACameraManager_create"},
    {"libvulkan.so", "vkCreateInstance"},
    {"libsync.so", "sync_wait"},
    {"libneuralnetworks.so", "ANeuralNetworksModel_create"},
    {"libbinder_ndk.so", "AIBinder_Class_define"},
    {"libstdc++.so", "__cxa_guard_acquire"},
};
constexpr size_t kPlatformLibCount = sizeof(kPlatformLibs) / sizeof(kPlatformLibs[0]);
// Each handle is the address of one of these bytes.
char g_platform_handles[kPlatformLibCount];

std::mutex g_dirs_mutex;
std::vector<std::string> g_dirs;

}  // namespace

// --- shared (extra_bionic.h) --------------------------------------------------------

void* PlatformLibraryHandle(const char* path) {
  if (path == nullptr) return nullptr;
  const char* slash = std::strrchr(path, '/');
  const char* base = slash != nullptr ? slash + 1 : path;
  if (slash != nullptr) {
    static const char* const kSystemDirs[] = {"/system/", "/vendor/", "/apex/", "/product/",
                                              "/odm/",    "/system_ext/"};
    bool system = false;
    for (const char* d : kSystemDirs) system |= std::strncmp(path, d, std::strlen(d)) == 0;
    if (!system) return nullptr;
  }
  for (size_t i = 0; i < kPlatformLibCount; ++i) {
    if (std::strcmp(base, kPlatformLibs[i].soname) == 0) {
      bool available = LookupBionicSymbol(kPlatformLibs[i].probe) != nullptr;
      // The standalone native provider implements the font portion of
      // libandroid without the framework AssetManager/JNI provider. As for
      // every platform handle, unavailable individual symbols still fail
      // through the ordinary dlsym lookup.
      if (std::strcmp(base, "libandroid.so") == 0)
        available |= LookupBionicSymbol("ASystemFontIterator_open") != nullptr;
      return available ? &g_platform_handles[i] : nullptr;
    }
  }
  return nullptr;
}

bool IsPlatformLibraryHandle(const void* handle) {
  auto p = static_cast<const char*>(handle);
  return p >= g_platform_handles && p < g_platform_handles + kPlatformLibCount;
}

const char* PlatformLibraryName(const void* handle) {
  if (!IsPlatformLibraryHandle(handle)) return "";
  return kPlatformLibs[static_cast<const char*>(handle) - g_platform_handles].soname;
}

void NoteLibraryDirectory(const std::string& dir) {
  if (dir.empty()) return;
  std::lock_guard<std::mutex> lock(g_dirs_mutex);
  auto it = std::find(g_dirs.begin(), g_dirs.end(), dir);
  if (it != g_dirs.end()) g_dirs.erase(it);
  g_dirs.insert(g_dirs.begin(), dir);
}

std::vector<std::string> LibraryDirectories() {
  std::vector<std::string> out;
  {
    std::lock_guard<std::mutex> lock(g_dirs_mutex);
    out = g_dirs;
  }
  if (const char* lp = std::getenv("LD_LIBRARY_PATH")) {
    std::string p = lp;
    size_t start = 0;
    while (start <= p.size()) {
      size_t colon = p.find(':', start);
      std::string d = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
      if (!d.empty() && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
      if (colon == std::string::npos) break;
      start = colon + 1;
    }
  }
  return out;
}

void SetPropertyAccessors(PropertyGetter get, PropertySetter set) {
  std::lock_guard<std::mutex> lock(g_prop_mutex);
  g_prop_get = get;
  g_prop_set = set;
}

int SystemPropertyGetValue(const char* name, char* value) {
  std::string v;
  if (!GetProp(name, &v)) v.clear();
  if (name && std::strncmp(name, "ro.", 3) == 0 && v.size() >= 92) v = properties::kLongLegacyError;
  if (v.size() > 91) v.resize(91);  // PROP_VALUE_MAX - 1
  if (value != nullptr) std::memcpy(value, v.c_str(), v.size() + 1);
  return static_cast<int>(v.size());
}

// The trampolines for the variadic wide forms (GESHER_VA_TRAMPOLINE).
asm(".text\n"
    GESHER_VA_TRAMPOLINE("gesher_extra_wprintf", "gesher_extra_vwprintf", "8", "%rsi")
    GESHER_VA_TRAMPOLINE("gesher_extra_fwprintf", "gesher_extra_vfwprintf", "16", "%rdx"));
extern "C" int gesher_extra_wprintf();
extern "C" int gesher_extra_fwprintf();

void RegisterExtra(SymbolTable& t) {
  // libm.
  t["acosh"] = WRAPS(double (*)(double), ::acosh);
  t["asinh"] = WRAPS(double (*)(double), ::asinh);
  t["atanh"] = WRAPS(double (*)(double), ::atanh);
  t["erf"] = WRAPS(double (*)(double), ::erf);
  t["erfc"] = WRAPS(double (*)(double), ::erfc);
  t["lgamma"] = WRAPS(double (*)(double), ::lgamma);
  t["gamma"] = WRAPS(double (*)(double), ::lgamma);
  t["tgamma"] = WRAPS(double (*)(double), ::tgamma);
  t["logb"] = WRAPS(double (*)(double), ::logb);
  t["ilogb"] = WRAPS(int (*)(double), ::ilogb);
  t["nextafter"] = WRAPS(double (*)(double, double), ::nextafter);
  t["fdim"] = WRAPS(double (*)(double, double), ::fdim);
  t["drem"] = WRAPS(double (*)(double, double), ::remainder);
  t["fma"] = WRAPS(double (*)(double, double, double), ::fma);
  t["remquo"] = WRAPS(double (*)(double, double, int*), ::remquo);
  t["frexpf"] = WRAPS(float (*)(float, int*), ::frexpf);
  t["modff"] = WRAPS(float (*)(float, float*), ::modff);
  t["acoshf"] = WRAPS(float (*)(float), ::acoshf);
  t["asinhf"] = WRAPS(float (*)(float), ::asinhf);
  t["atanhf"] = WRAPS(float (*)(float), ::atanhf);
  t["sinhf"] = WRAPS(float (*)(float), ::sinhf);
  t["coshf"] = WRAPS(float (*)(float), ::coshf);
  t["erff"] = WRAPS(float (*)(float), ::erff);
  t["erfcf"] = WRAPS(float (*)(float), ::erfcf);
  t["lgammaf"] = WRAPS(float (*)(float), ::lgammaf);
  t["gammaf"] = WRAPS(float (*)(float), ::lgammaf);
  t["tgammaf"] = WRAPS(float (*)(float), ::tgammaf);
  t["expm1f"] = WRAPS(float (*)(float), ::expm1f);
  t["log1pf"] = WRAPS(float (*)(float), ::log1pf);
  t["logbf"] = WRAPS(float (*)(float), ::logbf);
  t["ilogbf"] = WRAPS(int (*)(float), ::ilogbf);
  t["nearbyintf"] = WRAPS(float (*)(float), ::nearbyintf);
  t["nextafterf"] = WRAPS(float (*)(float, float), ::nextafterf);
  t["remainderf"] = WRAPS(float (*)(float, float), ::remainderf);
  t["fdimf"] = WRAPS(float (*)(float, float), ::fdimf);
  t["fmaf"] = WRAPS(float (*)(float, float, float), ::fmaf);
  t["remquof"] = WRAPS(float (*)(float, float, int*), ::remquof);
  E("scalbln", Scalbln);
  E("scalblnf", Scalblnf);
  E("nan", Nan);
  E("nanf", Nanf);
  E("lgamma_r", LgammaR);
  E("lgammaf_r", LgammafR);
  E("significand", Significand);
  E("significandf", Significandf);
  E("exp10", Exp10);
  E("exp10f", Exp10f);
  E("pow10", Exp10);
  E("pow10f", Exp10f);
  E("j0", J0);
  E("j1", J1);
  E("jn", Jn);
  E("y0", Y0);
  E("y1", Y1);
  E("yn", Yn);
  E("j0f", J0f);
  E("j1f", J1f);
  E("jnf", Jnf);
  E("y0f", Y0f);
  E("y1f", Y1f);
  E("ynf", Ynf);
  E("isnan", IsNan);
  E("__isnan", IsNan);
  E("isnanf", IsNanf);
  E("__isnanf", IsNanf);
  E("isinf", IsInf);
  E("__isinf", IsInf);
  E("isinff", IsInff);
  E("__isinff", IsInff);
  E("finite", Finite);
  E("__isfinite", Finite);
  E("finitef", Finitef);
  E("__isfinitef", Finitef);
  E("__isnormal", IsNormal);
  E("__isnormalf", IsNormalf);
  E("__signbit", SignBit);
  E("__signbitf", SignBitf);
  E("__fpclassify", Fpclassify);
  E("__fpclassifyd", Fpclassify);
  E("__fpclassifyf", Fpclassifyf);
  // Floating-point environment (x86-64).
  E("fegetenv", Fegetenv);
  E("fesetenv", Fesetenv);
  E("fegetround", Fegetround);
  E("fesetround", Fesetround);
  E("fetestexcept", Fetestexcept);
  E("feclearexcept", Feclearexcept);
  E("feraiseexcept", Feraiseexcept);
  E("feholdexcept", Feholdexcept);
  E("feupdateenv", Feupdateenv);
  E("fegetexceptflag", Fegetexceptflag);
  E("fesetexceptflag", Fesetexceptflag);
  E("feenableexcept", Feenableexcept);
  E("fedisableexcept", Fedisableexcept);
  E("fegetexcept", Fegetexcept);
  t["__fe_dfl_env"] = const_cast<X64Fenv*>(&kDefaultFenv);
  // stdlib.
  E("div", Div);
  E("ldiv", Ldiv);
  E("lldiv", Ldiv);
  E("imaxdiv", Ldiv);
  E("imaxabs", Imaxabs);
  E("rand_r", RandR);
  E("drand48", Drand48);
  E("erand48", Erand48);
  E("nrand48", Nrand48);
  E("mrand48", Mrand48);
  E("jrand48", Jrand48);
  E("seed48", Seed48);
  E("lcong48", Lcong48);
  E("getopt", Getopt);
  E("getopt_long", GetoptLong);
  E("getopt_long_only", GetoptLongOnly);
  t["optarg"] = &g_optarg;
  t["optind"] = &g_optind;
  t["opterr"] = &g_opterr;
  t["optopt"] = &g_optopt;
  t["optreset"] = &g_optreset;
  E("mkstemp", Mkstemp);
  E("mkstemp64", Mkstemp);
  E("mkostemp", Mkostemp);
  E("mkostemp64", Mkostemp);
  E("mkstemps", Mkstemps);
  E("mkstemps64", Mkstemps);
  E("mkostemps", Mkostemps);
  E("mkostemps64", Mkostemps);
  E("mkdtemp", Mkdtemp);
  E("mktemp", Mktemp);
  E("getpwuid", Getpwuid);
  E("getpwnam", Getpwnam);
  E("getpwuid_r", GetpwuidR);
  E("getpwnam_r", GetpwnamR);
  E("getgrgid", Getgrgid);
  E("getgrnam", Getgrnam);
  E("getlogin", Getlogin);
  E("getlogin_r", GetloginR);
  E("getpwent", Getpwent);
  E("setpwent", Setpwent);
  E("endpwent", Endpwent);
  E("getgrent", Getpwent);
  E("setgrent", Setpwent);
  E("endgrent", Endpwent);
  E("getloadavg", Getloadavg);
  E("at_quick_exit", AtQuickExit);
  E("quick_exit", QuickExit);
  // Strings.
  E("memccpy", Memccpy);
  E("mempcpy", Mempcpy);
  E("bzero", Bzero);
  E("explicit_bzero", ExplicitBzero);
  E("bcmp", Bcmp);
  E("bcopy", Bcopy);
  E("ffs", Ffs);
  E("ffsl", Ffsl);
  E("ffsll", Ffsl);
  E("swab", Swab);
  E("strxfrm", Strxfrm);
  E("strverscmp", Strverscmp);
  E("strerror_l", StrerrorL);
  E("strcasecmp_l", StrcasecmpL);
  E("strncasecmp_l", StrncasecmpL);
  E("toupper_l", ToupperL);
  E("tolower_l", TolowerL);
  E("isalpha_l", isalpha_L);
  E("isdigit_l", isdigit_L);
  E("isalnum_l", isalnum_L);
  E("isspace_l", isspace_L);
  E("isupper_l", isupper_L);
  E("islower_l", islower_L);
  E("isxdigit_l", isxdigit_L);
  E("isprint_l", isprint_L);
  E("ispunct_l", ispunct_L);
  E("iscntrl_l", iscntrl_L);
  E("isgraph_l", isgraph_L);
  E("isblank", Isblank);
  E("isblank_l", IsblankL);
  E("isascii", Isascii);
  E("toascii", Toascii);
  E("strtod_l", StrtodL);
  E("strtof_l", StrtofL);
  E("strtol_l", StrtolL);
  E("strtoul_l", StrtoulL);
  // Wide characters.
  E("iswalpha", W_iswalpha);
  E("iswalnum", W_iswalnum);
  E("iswdigit", W_iswdigit);
  E("iswxdigit", W_iswxdigit);
  E("iswspace", W_iswspace);
  E("iswupper", W_iswupper);
  E("iswlower", W_iswlower);
  E("iswcntrl", W_iswcntrl);
  E("iswpunct", W_iswpunct);
  E("iswprint", W_iswprint);
  E("iswgraph", W_iswgraph);
  E("iswblank", W_iswblank);
  E("towlower", W_towlower);
  E("towupper", W_towupper);
  E("wctype", Wctype);
  E("wctype_l", WctypeL);
  E("iswctype", Iswctype);
  E("iswctype_l", IswctypeL);
  E("wctrans", Wctrans);
  E("towctrans", Towctrans);
  E("wctrans_l", WctransL);
  E("towctrans_l", TowctransL);
  E("wcwidth", Wcwidth);
  E("wcswidth", Wcswidth);
  E("wcscmp", Wcscmp);
  E("wcsncmp", Wcsncmp);
  E("wcscasecmp", Wcscasecmp);
  E("wcsncasecmp", Wcsncasecmp);
  E("wcscasecmp_l", WcscasecmpL);
  E("wcsncasecmp_l", WcsncasecmpL);
  E("wcschr", Wcschr);
  E("wcsrchr", Wcsrchr);
  E("wcsstr", Wcsstr);
  E("wcscpy", Wcscpy);
  E("wcpcpy", Wcpcpy);
  E("wcsncpy", Wcsncpy);
  E("wcpncpy", Wcpncpy);
  E("wcscat", Wcscat);
  E("wcsncat", Wcsncat);
  E("wcslcpy", Wcslcpy);
  E("wcslcat", Wcslcat);
  E("wcsnlen", Wcsnlen);
  E("wcsdup", Wcsdup);
  E("wcsspn", Wcsspn);
  E("wcscspn", Wcscspn);
  E("wcspbrk", Wcspbrk);
  E("wcstok", Wcstok);
  E("wmemcpy", Wmemcpy);
  E("wmempcpy", Wmempcpy);
  E("wmemmove", Wmemmove);
  E("wmemset", Wmemset);
  t["wprintf"] = reinterpret_cast<void*>(&gesher_extra_wprintf);
  t["fwprintf"] = reinterpret_cast<void*>(&gesher_extra_fwprintf);
  t["vwprintf"] = reinterpret_cast<void*>(&gesher_extra_vwprintf);
  t["vfwprintf"] = reinterpret_cast<void*>(&gesher_extra_vfwprintf);
  E("fgetws", Fgetws);
  // stdio.
  E("flockfile", Flockfile);
  E("funlockfile", Funlockfile);
  E("ftrylockfile", Ftrylockfile);
  E("getc_unlocked", GetcUnlocked);
  E("fgetc_unlocked", GetcUnlocked);
  E("getchar_unlocked", GetcharUnlocked);
  E("putc_unlocked", PutcUnlocked);
  E("fputc_unlocked", PutcUnlocked);
  E("putchar_unlocked", PutcharUnlocked);
  E("fread_unlocked", FreadUnlocked);
  E("fwrite_unlocked", FwriteUnlocked);
  E("fputs_unlocked", FputsUnlocked);
  E("fgets_unlocked", FgetsUnlocked);
  E("fflush_unlocked", FflushUnlocked);
  E("feof_unlocked", FeofUnlocked);
  E("ferror_unlocked", FerrorUnlocked);
  E("clearerr_unlocked", ClearerrUnlocked);
  E("fileno_unlocked", FilenoUnlocked);
  E("__fsetlocking", Fsetlocking);
  E("setlinebuf", Setlinebuf);
  E("setbuffer", Setbuffer);
  E("getw", Getw);
  E("putw", Putw);
  E("fmemopen", Fmemopen);
  E("ctermid", Ctermid);
  E("tmpnam", Tmpnam);
  E("tempnam", Tempnam);
  // Time.
  E("strptime", Strptime);
  E("strptime_l", StrptimeL);
  // System properties.
  E("__system_property_get", SystemPropertyGet);
  E("__system_property_set", SystemPropertySet);
  E("__system_property_find", SystemPropertyFind);
  E("__system_property_read", SystemPropertyRead);
  E("__system_property_read_callback", SystemPropertyReadCallback);
  E("__system_property_foreach", SystemPropertyForeach);
  E("__system_property_serial", SystemPropertySerial);
  E("__system_property_area_serial", SystemPropertyAreaSerial);
  E("__system_property_wait", SystemPropertyWait);
  E("__system_property_wait_any", SystemPropertyWaitAny);
  E("__system_property_find_nth", SystemPropertyFindNth);
  E("android_get_device_api_level", GetDeviceApiLevel);
  E("android_get_application_target_sdk_version", GetTargetSdk);
  E("android_set_application_target_sdk_version", SetTargetSdk);
  E("android_get_LD_LIBRARY_PATH", GetLdLibraryPath);
  E("android_update_LD_LIBRARY_PATH", UpdateLdLibraryPath);

  RegisterExtraSys(t);
  RegisterZlib(t);
}

}  // namespace gesher::bionic
