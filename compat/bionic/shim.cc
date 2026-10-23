// gesher bionic shim: the libc/libm/libdl/liblog symbols an app's .so imports,
// as System V entry points over gesher-base and the CRT.
//
// A CRT or gesher-base function whose signature is the same under bionic's
// LP64 ABI is exposed through a generated sysv_abi wrapper (Sysv<F>). Anything
// whose types differ is written out: `long` is 64-bit in the .so and 32-bit
// here (strtol, lseek, sysconf, fseek, ...), struct timespec's tv_nsec is
// 64-bit there, and callbacks the .so passes in (qsort's comparator) are
// System V. The variadic stdio and logging entry points are in
// stdio_bionic.cc, pthread in pthread_bionic.cc.
//
// InstallBionicShim() registers the resolver with the ELF loader.

#include "shim_internal.h"

#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <windows.h>
// bcrypt.h needs windows.h first.
#include <bcrypt.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gesher::bionic {

namespace {

// A sysv_abi wrapper around a function F whose signature means the same to the
// .so (CRT or gesher-base).
template <auto F>
struct Sysv;
template <class R, class... A, R (*F)(A...)>
struct Sysv<F> {
  static R BIONIC_SYSV Call(A... args) { return F(args...); }
};
#define WRAP(fn) reinterpret_cast<void*>(&Sysv<&fn>::Call)
// For an overloaded function (the math ones): select the overload by signature.
#define WRAPS(sig, fn) reinterpret_cast<void*>(&Sysv<static_cast<sig>(&fn)>::Call)
#define ENTRY(name, ptr) (t[name] = reinterpret_cast<void*>(ptr))
#define SELF(fn) ENTRY(#fn, WRAP(::fn))
#define MATH(fn) ENTRY(#fn, WRAPS(double (*)(double), ::fn))
#define MATH2(fn) ENTRY(#fn, WRAPS(double (*)(double, double), ::fn))
#define MATHF(fn) ENTRY(#fn, WRAPS(float (*)(float), ::fn))
#define MATH2F(fn) ENTRY(#fn, WRAPS(float (*)(float, float), ::fn))

// --- memory ----------------------------------------------------------------
// malloc is the CRT's (16-byte aligned on x64, as bionic's). An allocation
// with a larger alignment is over-allocated; free and realloc recognise it
// through a side table, so every pointer the .so gets can go to free().

struct Aligned {
  void* raw;
  size_t size;
};
std::mutex g_aligned_mutex;
std::unordered_map<void*, Aligned> g_aligned;
std::atomic<size_t> g_aligned_count{0};

bool TakeAligned(void* p, Aligned* out) {
  if (g_aligned_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(g_aligned_mutex);
  auto it = g_aligned.find(p);
  if (it == g_aligned.end()) return false;
  *out = it->second;
  g_aligned.erase(it);
  g_aligned_count.fetch_sub(1, std::memory_order_release);
  return true;
}

void* AlignedAlloc(size_t align, size_t size) {
  if (align <= 16) return std::malloc(size ? size : 1);
  void* raw = std::malloc(size + align);
  if (raw == nullptr) return nullptr;
  auto p = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + align) & ~(align - 1));
  std::lock_guard<std::mutex> lock(g_aligned_mutex);
  g_aligned[p] = {raw, size};
  g_aligned_count.fetch_add(1, std::memory_order_release);
  return p;
}

void* BIONIC_SYSV Malloc(size_t n) { return std::malloc(n ? n : 1); }
void* BIONIC_SYSV Calloc(size_t n, size_t m) { return std::calloc(n ? n : 1, m ? m : 1); }
void BIONIC_SYSV Free(void* p) {
  Aligned a;
  if (TakeAligned(p, &a)) {
    std::free(a.raw);
  } else {
    std::free(p);
  }
}
void* BIONIC_SYSV Realloc(void* p, size_t n) {
  Aligned a;
  if (p != nullptr && TakeAligned(p, &a)) {
    void* q = std::malloc(n ? n : 1);
    if (q != nullptr) std::memcpy(q, p, a.size < n ? a.size : n);
    std::free(a.raw);
    return q;
  }
  return std::realloc(p, n);
}
int BIONIC_SYSV PosixMemalign(void** out, size_t align, size_t size) {
  if (align < sizeof(void*) || (align & (align - 1)) != 0) return kEINVAL;
  void* p = AlignedAlloc(align, size);
  if (p == nullptr) return kENOMEM;
  *out = p;
  return 0;
}
void* BIONIC_SYSV Memalign(size_t align, size_t size) {
  if (align == 0 || (align & (align - 1)) != 0) align = 16;
  return AlignedAlloc(align, size);
}
size_t BIONIC_SYSV MallocUsableSize(const void* p) {
  if (p == nullptr) return 0;
  if (g_aligned_count.load(std::memory_order_acquire) != 0) {
    std::lock_guard<std::mutex> lock(g_aligned_mutex);
    auto it = g_aligned.find(const_cast<void*>(p));
    if (it != g_aligned.end()) return it->second.size;
  }
  return _msize(const_cast<void*>(p));
}

// --- strings and numbers with `long` --------------------------------------

int64_t BIONIC_SYSV Strtol(const char* s, char** end, int base) { return std::strtoll(s, end, base); }
uint64_t BIONIC_SYSV Strtoul(const char* s, char** end, int base) {
  return std::strtoull(s, end, base);
}
int64_t BIONIC_SYSV Atol(const char* s) { return std::atoll(s); }
int64_t BIONIC_SYSV Labs(int64_t v) { return v < 0 ? -v : v; }
int64_t BIONIC_SYSV Lround(double v) { return std::llround(v); }
int64_t BIONIC_SYSV Lroundf(float v) { return ::llroundf(v); }
int64_t BIONIC_SYSV Lrint(double v) { return std::llrint(v); }
int64_t BIONIC_SYSV Lrintf(float v) { return ::llrintf(v); }
double BIONIC_SYSV Ldexp(double v, int e) { return std::ldexp(v, e); }
double BIONIC_SYSV Frexp(double v, int* e) { return std::frexp(v, e); }
double BIONIC_SYSV Modf(double v, double* i) { return std::modf(v, i); }
float BIONIC_SYSV Ldexpf(float v, int e) { return std::ldexp(v, e); }

char* BIONIC_SYSV Strdup(const char* s) {
  size_t n = std::strlen(s) + 1;
  char* p = static_cast<char*>(std::malloc(n));
  if (p) std::memcpy(p, s, n);
  return p;
}
char* BIONIC_SYSV Strndup(const char* s, size_t max) {
  size_t n = strnlen(s, max);
  char* p = static_cast<char*>(std::malloc(n + 1));
  if (p) {
    std::memcpy(p, s, n);
    p[n] = 0;
  }
  return p;
}
size_t BIONIC_SYSV Strlcpy(char* dst, const char* src, size_t size) {
  size_t n = std::strlen(src);
  if (size != 0) {
    size_t c = n < size - 1 ? n : size - 1;
    std::memcpy(dst, src, c);
    dst[c] = 0;
  }
  return n;
}
size_t BIONIC_SYSV Strlcat(char* dst, const char* src, size_t size) {
  size_t d = strnlen(dst, size);
  if (d == size) return size + std::strlen(src);
  return d + Strlcpy(dst + d, src, size - d);
}
char* BIONIC_SYSV StrtokR(char* s, const char* delim, char** save) {
  return strtok_r(s, delim, save);
}
// The C++ headers overload these on constness; the .so wants the C ones.
void* BIONIC_SYSV Memchr(const void* s, int c, size_t n) {
  return const_cast<void*>(std::memchr(s, c, n));
}
char* BIONIC_SYSV Strchr(const char* s, int c) { return const_cast<char*>(std::strchr(s, c)); }
char* BIONIC_SYSV Strrchr(const char* s, int c) { return const_cast<char*>(std::strrchr(s, c)); }
char* BIONIC_SYSV Strstr(const char* h, const char* n) {
  return const_cast<char*>(std::strstr(h, n));
}
char* BIONIC_SYSV Strpbrk(const char* s, const char* a) {
  return const_cast<char*>(std::strpbrk(s, a));
}
const char* BIONIC_SYSV Strerror(int e) { return strerror(e); }
int BIONIC_SYSV StrerrorR(int e, char* buf, size_t n) {
  if (n == 0) return kEINVAL;
  Strlcpy(buf, strerror(e), n);
  return 0;
}
int BIONIC_SYSV Strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }
int BIONIC_SYSV Strncasecmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }
void* BIONIC_SYSV Memmem(const void* h, size_t hn, const void* n, size_t nn) {
  if (nn == 0) return const_cast<void*>(h);
  auto hp = static_cast<const uint8_t*>(h);
  for (size_t i = 0; i + nn <= hn; ++i) {
    if (std::memcmp(hp + i, n, nn) == 0) return const_cast<uint8_t*>(hp + i);
  }
  return nullptr;
}

// FORTIFY (_FORTIFY_SOURCE) checked variants: check, then do the work.
[[noreturn]] void FortifyFail(const char* what) {
  std::fprintf(stderr, "gesher bionic: FORTIFY: %s: buffer overflow detected\n", what);
  std::abort();
}
void* BIONIC_SYSV MemcpyChk(void* d, const void* s, size_t n, size_t dn) {
  if (n > dn) FortifyFail("memcpy");
  return std::memcpy(d, s, n);
}
void* BIONIC_SYSV MemmoveChk(void* d, const void* s, size_t n, size_t dn) {
  if (n > dn) FortifyFail("memmove");
  return std::memmove(d, s, n);
}
void* BIONIC_SYSV MemsetChk(void* d, int c, size_t n, size_t dn) {
  if (n > dn) FortifyFail("memset");
  return std::memset(d, c, n);
}
size_t BIONIC_SYSV StrlenChk(const char* s, size_t sn) {
  size_t n = strnlen(s, sn);
  if (n >= sn) FortifyFail("strlen");
  return n;
}
char* BIONIC_SYSV StrcpyChk(char* d, const char* s, size_t dn) {
  if (std::strlen(s) >= dn) FortifyFail("strcpy");
  return std::strcpy(d, s);
}
char* BIONIC_SYSV StrcatChk(char* d, const char* s, size_t dn) {
  if (std::strlen(d) + std::strlen(s) >= dn) FortifyFail("strcat");
  return std::strcat(d, s);
}
char* BIONIC_SYSV StrncpyChk(char* d, const char* s, size_t n, size_t dn) {
  if (n > dn) FortifyFail("strncpy");
  return std::strncpy(d, s, n);
}
char* BIONIC_SYSV StrncpyChk2(char* d, const char* s, size_t n, size_t dn, size_t /*sn*/) {
  return StrncpyChk(d, s, n, dn);
}
char* BIONIC_SYSV StrncatChk(char* d, const char* s, size_t n, size_t dn) {
  if (std::strlen(d) + strnlen(s, n) >= dn) FortifyFail("strncat");
  return std::strncat(d, s, n);
}
const char* BIONIC_SYSV StrchrChk(const char* s, int c, size_t /*sn*/) { return std::strchr(s, c); }
const char* BIONIC_SYSV StrrchrChk(const char* s, int c, size_t /*sn*/) {
  return std::strrchr(s, c);
}
size_t BIONIC_SYSV StrlcpyChk(char* d, const char* s, size_t n, size_t dn) {
  if (n > dn) FortifyFail("strlcpy");
  return Strlcpy(d, s, n);
}
size_t BIONIC_SYSV StrlcatChk(char* d, const char* s, size_t n, size_t dn) {
  if (n > dn) FortifyFail("strlcat");
  return Strlcat(d, s, n);
}

// --- qsort / bsearch with a System V comparator ----------------------------

using SysvCompare = int (BIONIC_SYSV*)(const void*, const void*);
int __cdecl CompareThunk(void* ctx, const void* a, const void* b) {
  return reinterpret_cast<SysvCompare>(ctx)(a, b);
}
void BIONIC_SYSV Qsort(void* base, size_t n, size_t size, SysvCompare cmp) {
  qsort_s(base, n, size, CompareThunk, reinterpret_cast<void*>(cmp));
}
void* BIONIC_SYSV Bsearch(const void* key, const void* base, size_t n, size_t size,
                          SysvCompare cmp) {
  auto lo = static_cast<const uint8_t*>(base);
  while (n > 0) {
    const uint8_t* mid = lo + (n / 2) * size;
    int c = cmp(key, mid);
    if (c == 0) return const_cast<uint8_t*>(mid);
    if (c > 0) {
      lo = mid + size;
      n = n - n / 2 - 1;
    } else {
      n /= 2;
    }
  }
  return nullptr;
}

// --- errno, process, environment ------------------------------------------

// bionic's errno is *__errno(); gesher-base keeps Linux's numbers in the
// CRT's errno, so it is the same object.
int* BIONIC_SYSV Errno() { return &errno; }

[[noreturn]] void BIONIC_SYSV Abort() {
  std::fflush(stdout);
  std::fflush(stderr);
  std::abort();
}
[[noreturn]] void BIONIC_SYSV Exit(int code) { std::exit(code); }
[[noreturn]] void BIONIC_SYSV UnderscoreExit(int code) {
  std::fflush(stdout);
  std::fflush(stderr);
  ExitProcess(static_cast<UINT>(code));
}

// bionic's assert: __assert2(file, line, function, expression).
[[noreturn]] void BIONIC_SYSV Assert2(const char* file, int line, const char* func,
                                      const char* expr) {
  std::fprintf(stderr, "%s:%d: %s: assertion \"%s\" failed\n", file, line, func, expr);
  Abort();
}
[[noreturn]] void BIONIC_SYSV Assert(const char* file, int line, const char* expr) {
  std::fprintf(stderr, "%s:%d: assertion \"%s\" failed\n", file, line, expr);
  Abort();
}
void BIONIC_SYSV SetAbortMessage(const char* msg) {
  std::fprintf(stderr, "gesher bionic: abort message: %s\n", msg ? msg : "");
}

char* BIONIC_SYSV Getenv(const char* name) { return std::getenv(name); }
int BIONIC_SYSV Setenv(const char* name, const char* value, int overwrite) {
  if (!overwrite && std::getenv(name) != nullptr) return 0;
  return _putenv_s(name, value) == 0 ? 0 : -1;
}
int BIONIC_SYSV Unsetenv(const char* name) { return _putenv_s(name, "") == 0 ? 0 : -1; }

int BIONIC_SYSV Getpid() { return static_cast<int>(GetCurrentProcessId()); }
int BIONIC_SYSV Gettid() { return static_cast<int>(GetCurrentThreadId()); }
int BIONIC_SYSV Getpagesize() { return 4096; }

// Android system properties: none are set; reads return the empty string.
int BIONIC_SYSV SystemPropertyGet(const char* /*name*/, char* value) {
  if (value) value[0] = 0;
  return 0;
}

// rand/random: the CRT's rand is 15 bits; random() promises 31.
uint32_t g_random_state = 1;
std::mutex g_random_mutex;
int64_t BIONIC_SYSV Random() {
  std::lock_guard<std::mutex> lock(g_random_mutex);
  g_random_state = g_random_state * 1103515245u + 12345u;
  uint32_t a = g_random_state;
  g_random_state = g_random_state * 1103515245u + 12345u;
  return static_cast<int64_t>(((a >> 16) << 15) ^ (g_random_state >> 16)) & 0x7fffffff;
}
void BIONIC_SYSV Srandom(unsigned seed) {
  std::lock_guard<std::mutex> lock(g_random_mutex);
  g_random_state = seed;
}
int BIONIC_SYSV Rand() { return static_cast<int>(Random()); }
void BIONIC_SYSV ArcRandomBuf(void* buf, size_t n) {
  BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}
uint32_t BIONIC_SYSV ArcRandom() {
  uint32_t v;
  ArcRandomBuf(&v, sizeof(v));
  return v;
}
uint32_t BIONIC_SYSV ArcRandomUniform(uint32_t bound) {
  if (bound < 2) return 0;
  uint32_t min = (0u - bound) % bound;
  for (;;) {
    uint32_t r = ArcRandom();
    if (r >= min) return r % bound;
  }
}

// --- time ------------------------------------------------------------------

int64_t QpcNanos() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<int64_t>((static_cast<__int128>(now.QuadPart) * 1000000000) / freq.QuadPart);
}

int64_t ThreadCpuNanos() {
  FILETIME c, e, k, u;
  GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
  auto v = [](FILETIME f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (v(k) + v(u)) * 100;
}
int64_t ProcessCpuNanos() {
  FILETIME c, e, k, u;
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  auto v = [](FILETIME f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (v(k) + v(u)) * 100;
}

int BIONIC_SYSV ClockGettime(int clock, LinuxTimespec* ts) {
  int64_t ns;
  switch (clock) {
    case 0:   // CLOCK_REALTIME
    case 5: {  // CLOCK_REALTIME_COARSE
      *ts = RealtimeNow();
      return 0;
    }
    case 1: case 4: case 6: case 7:  // MONOTONIC, _RAW, _COARSE, BOOTTIME
      ns = QpcNanos();
      break;
    case 2:  // CLOCK_PROCESS_CPUTIME_ID
      ns = ProcessCpuNanos();
      break;
    case 3:  // CLOCK_THREAD_CPUTIME_ID
      ns = ThreadCpuNanos();
      break;
    default:
      errno = kEINVAL;
      return -1;
  }
  ts->tv_sec = ns / 1000000000;
  ts->tv_nsec = ns % 1000000000;
  return 0;
}
int BIONIC_SYSV ClockGetres(int, LinuxTimespec* ts) {
  if (ts) {
    ts->tv_sec = 0;
    ts->tv_nsec = 100;
  }
  return 0;
}
struct LinuxTimeval {
  int64_t tv_sec;
  int64_t tv_usec;
};
int BIONIC_SYSV Gettimeofday(LinuxTimeval* tv, void*) {
  if (tv) {
    LinuxTimespec ts = RealtimeNow();
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
  }
  return 0;
}
int64_t BIONIC_SYSV Time(int64_t* out) {
  int64_t t = RealtimeNow().tv_sec;
  if (out) *out = t;
  return t;
}
int BIONIC_SYSV Nanosleep(const LinuxTimespec* req, LinuxTimespec* rem) {
  int64_t ms = req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000;
  DeliverPendingSignals();
  Sleep(static_cast<DWORD>(ms));
  if (rem) *rem = {0, 0};
  DeliverPendingSignals();
  return 0;
}
int BIONIC_SYSV Usleep(uint32_t us) {
  DeliverPendingSignals();
  Sleep((us + 999) / 1000);
  DeliverPendingSignals();
  return 0;
}
unsigned BIONIC_SYSV SleepSeconds(unsigned s) {
  DeliverPendingSignals();
  Sleep(s * 1000);
  DeliverPendingSignals();
  return 0;
}
int64_t BIONIC_SYSV Clock() { return ProcessCpuNanos() / 1000; }  // CLOCKS_PER_SEC 1e6

// --- files (fd level) -----------------------------------------------------

// open(path, flags, ...): the mode is the third register whether or not the
// caller passed it, and open ignores it without O_CREAT. Linux flag values,
// like gesher-base's open.
int BIONIC_SYSV Open(const char* path, int flags, int mode) { return open(path, flags, mode); }
int BIONIC_SYSV Close(int fd) { return close(fd); }
int64_t BIONIC_SYSV Read(int fd, void* buf, size_t n) {
  return read(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
}
int64_t BIONIC_SYSV Write(int fd, const void* buf, size_t n) {
  return write(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
}
int64_t BIONIC_SYSV Pread(int fd, void* buf, size_t n, int64_t off) {
  return pread(fd, buf, n, static_cast<off_t>(off));
}
int64_t BIONIC_SYSV Pwrite(int fd, const void* buf, size_t n, int64_t off) {
  return pwrite(fd, buf, n, static_cast<off_t>(off));
}
int64_t BIONIC_SYSV Lseek(int fd, int64_t off, int whence) {
  return static_cast<int64_t>(lseek(fd, static_cast<off_t>(off), whence));
}
int BIONIC_SYSV Unlink(const char* path) { return unlink(path); }
int BIONIC_SYSV Access(const char* path, int mode) { return access(path, mode); }
int BIONIC_SYSV Mkdir(const char* path, uint32_t mode) {
  return mkdir(path, static_cast<mode_t>(mode));
}
int BIONIC_SYSV Dup(int fd) { return dup(fd); }
int BIONIC_SYSV Dup2(int a, int b) { return dup2(a, b); }
int BIONIC_SYSV Isatty(int fd) { return isatty(fd); }

// gesher-base's struct stat has Linux's layout, except that its timespecs'
// tv_nsec is the CRT's 32-bit long; widen them for the .so.
void FixStat(struct stat* st) {
  for (struct timespec* ts : {&st->st_atim, &st->st_mtim, &st->st_ctim}) {
    int64_t ns = ts->tv_nsec;
    std::memcpy(reinterpret_cast<uint8_t*>(ts) + 8, &ns, 8);
  }
}
int BIONIC_SYSV Stat(const char* path, struct stat* st) {
  std::memset(st, 0, sizeof(*st));
  int r = stat(path, st);
  if (r == 0) FixStat(st);
  return r;
}
int BIONIC_SYSV Lstat(const char* path, struct stat* st) {
  std::memset(st, 0, sizeof(*st));
  int r = lstat(path, st);
  if (r == 0) FixStat(st);
  return r;
}
int BIONIC_SYSV Fstat(int fd, struct stat* st) {
  std::memset(st, 0, sizeof(*st));
  int r = fstat(fd, st);
  if (r == 0) FixStat(st);
  return r;
}

void* BIONIC_SYSV Mmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
  return mmap(addr, len, prot, flags, fd, static_cast<off_t>(off));
}
int BIONIC_SYSV Munmap(void* addr, size_t len) { return munmap(addr, len); }
int BIONIC_SYSV Mprotect(void* addr, size_t len, int prot) { return mprotect(addr, len, prot); }

// --- C runtime support ------------------------------------------------------

// The stack-protector cookie, for a .so compiled with the global guard model
// (bionic x86-64 reads %fs:0x28 instead; see compat/elf/tls.cc).
uintptr_t g_stack_chk_guard = 0x0123456789abcdefULL;

[[noreturn]] void BIONIC_SYSV StackChkFail() {
  std::fprintf(stderr, "gesher bionic: stack corruption detected\n");
  Abort();
}

// __tls_get_addr(TlsIndex*): general-dynamic TLS.
struct TlsIndex {
  size_t module_id;
  size_t offset;
};
void* BIONIC_SYSV TlsGetAddr(TlsIndex* ti) {
  return gesher::elf::TlsGetAddr(ti->module_id, ti->offset);
}

// Static destructors (__cxa_atexit), run by __cxa_finalize(dso) for that
// object's entries, or all of them for nullptr.
struct AtExitEntry {
  void (BIONIC_SYSV* func)(void*);
  void* arg;
  void* dso;
  void (BIONIC_SYSV* plain)(void);  // atexit's, which takes no argument
};
std::mutex g_atexit_mutex;
std::vector<AtExitEntry> g_atexit;

int BIONIC_SYSV CxaAtexit(void (BIONIC_SYSV* func)(void*), void* arg, void* dso) {
  std::lock_guard<std::mutex> lock(g_atexit_mutex);
  g_atexit.push_back({func, arg, dso, nullptr});
  return 0;
}
void BIONIC_SYSV CxaFinalize(void* dso) {
  for (;;) {
    AtExitEntry e{};
    {
      std::lock_guard<std::mutex> lock(g_atexit_mutex);
      auto it = g_atexit.rbegin();
      while (it != g_atexit.rend() && dso != nullptr && it->dso != dso) ++it;
      if (it == g_atexit.rend()) return;
      e = *it;
      g_atexit.erase(std::next(it).base());
    }
    if (e.func) e.func(e.arg);
    if (e.plain) e.plain();
  }
}
int BIONIC_SYSV Atexit(void (BIONIC_SYSV* func)(void)) {
  std::lock_guard<std::mutex> lock(g_atexit_mutex);
  g_atexit.push_back({nullptr, nullptr, nullptr, func});
  return 0;
}

// --- libdl (through the ELF loader) ----------------------------------------

thread_local std::string t_dlerror;
thread_local bool t_has_dlerror = false;

void* BIONIC_SYSV DlOpen(const char* filename, int /*flags*/) {
  if (filename == nullptr) return reinterpret_cast<void*>(-2);  // the "main program"
  std::string error;
  const char* base = std::strrchr(filename, '/');
  if (gesher::elf::Library* existing = gesher::elf::Find(base ? base + 1 : filename)) {
    return existing;
  }
  gesher::elf::Library* lib = gesher::elf::Load(filename, &error);
  if (lib == nullptr) {
    t_dlerror = "dlopen failed: " + error;
    t_has_dlerror = true;
  }
  return lib;
}
void* BIONIC_SYSV DlSym(void* handle, const char* symbol) {
  void* p;
  // RTLD_DEFAULT is 0 and RTLD_NEXT -1 on bionic LP64.
  if (handle == nullptr || handle == reinterpret_cast<void*>(-1) ||
      handle == reinterpret_cast<void*>(-2)) {
    p = gesher::elf::LookupGlobal(symbol);
  } else {
    p = gesher::elf::Lookup(static_cast<gesher::elf::Library*>(handle), symbol);
  }
  if (p == nullptr) {
    t_dlerror = std::string("undefined symbol: ") + symbol;
    t_has_dlerror = true;
  }
  return p;
}
int BIONIC_SYSV DlClose(void*) { return 0; }
const char* BIONIC_SYSV DlError() {
  if (!t_has_dlerror) return nullptr;
  t_has_dlerror = false;
  return t_dlerror.c_str();
}

struct BionicDlInfo {
  const char* dli_fname;
  void* dli_fbase;
  const char* dli_sname;
  void* dli_saddr;
};
int BIONIC_SYSV DlAddr(const void* addr, BionicDlInfo* info) {
  gesher::elf::AddressInfo ai;
  if (!gesher::elf::FindAddress(addr, &ai)) return 0;
  info->dli_fname = ai.soname;
  info->dli_fbase = ai.base;
  info->dli_sname = ai.symbol;
  info->dli_saddr = ai.symbol_addr;
  return 1;
}

// bionic's struct dl_phdr_info (link.h, LP64).
struct BionicPhdrInfo {
  uint64_t dlpi_addr;
  const char* dlpi_name;
  const void* dlpi_phdr;
  uint16_t dlpi_phnum;
  uint64_t dlpi_adds;
  uint64_t dlpi_subs;
  uint64_t dlpi_tls_modid;
  void* dlpi_tls_data;
};
int BIONIC_SYSV DlIteratePhdr(int (BIONIC_SYSV* cb)(BionicPhdrInfo*, size_t, void*), void* data) {
  return gesher::elf::ForEachLoaded([&](const gesher::elf::PhdrInfo& in) {
    BionicPhdrInfo info{};
    info.dlpi_addr = in.addr;
    info.dlpi_name = in.name;
    info.dlpi_phdr = in.phdr;
    info.dlpi_phnum = in.phnum;
    info.dlpi_adds = in.adds;
    info.dlpi_subs = 0;
    info.dlpi_tls_modid = in.tls_modid;
    info.dlpi_tls_data = nullptr;
    return cb(&info, sizeof(info), data);
  });
}

// syscall(2), for what libc++/libc++abi use it for: futex waits and wakes
// (static-local guards, std::atomic::wait) and gettid. A variadic call passes
// the arguments in the same registers as this fixed signature reads.
int64_t BIONIC_SYSV Syscall(int64_t nr, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                            int64_t /*a5*/, int64_t /*a6*/) {
  constexpr int64_t kSysGettid = 186, kSysFutex = 202;
  if (nr == kSysGettid) return GetCurrentThreadId();
  if (nr != kSysFutex) {
    errno = kENOSYS;
    return -1;
  }
  // futex(uaddr, op, val, timeout): FUTEX_PRIVATE_FLAG (128) changes nothing
  // here; FUTEX_CLOCK_REALTIME (256) picks the clock of a *_BITSET timeout.
  auto* addr = reinterpret_cast<volatile int32_t*>(a1);
  int op = static_cast<int>(a2) & ~(128 | 256);
  bool realtime = (a2 & 256) != 0;
  auto* timeout = reinterpret_cast<const LinuxTimespec*>(a4);
  constexpr int kWait = 0, kWake = 1, kWaitBitset = 9, kWakeBitset = 10;
  if (op == kWait || op == kWaitBitset) {
    int32_t expect = static_cast<int32_t>(a3);
    DWORD ms = INFINITE;
    if (timeout != nullptr) {
      if (op == kWait) {  // relative
        int64_t t = timeout->tv_sec * 1000 + (timeout->tv_nsec + 999999) / 1000000;
        ms = t < 0 ? 0 : t > 0x7ffffffe ? 0x7ffffffe : static_cast<DWORD>(t);
      } else {  // absolute
        ms = MillisUntil(timeout, !realtime);
      }
    }
    if (*addr != expect) {
      errno = kEAGAIN;
      return -1;
    }
    if (!WaitOnAddress(const_cast<int32_t*>(addr), &expect, sizeof(expect), ms) &&
        GetLastError() == ERROR_TIMEOUT) {
      errno = kETIMEDOUT;
      return -1;
    }
    return 0;
  }
  if (op == kWake || op == kWakeBitset) {
    if (a3 == 1) {
      WakeByAddressSingle(const_cast<int32_t*>(addr));
    } else {
      WakeByAddressAll(const_cast<int32_t*>(addr));
    }
    return 0;
  }
  errno = kENOSYS;
  return -1;
}

void ReportUnresolved(const char* name) {
  std::fprintf(stderr, "gesher bionic: the .so called '%s', which the bionic shim lacks\n", name);
  std::fflush(stderr);
}

}  // namespace

// --- shared helpers (shim_internal.h) ---------------------------------------

LinuxTimespec RealtimeNow() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  int64_t t = ((static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) -
              116444736000000000LL;  // 1601 -> 1970, in 100 ns
  return {t / 10000000, (t % 10000000) * 100};
}

uint32_t MillisUntil(const LinuxTimespec* abstime, bool monotonic) {
  int64_t now_ns;
  if (monotonic) {
    now_ns = QpcNanos();
  } else {
    LinuxTimespec now = RealtimeNow();
    now_ns = now.tv_sec * 1000000000 + now.tv_nsec;
  }
  int64_t then_ns = abstime->tv_sec * 1000000000 + abstime->tv_nsec;
  if (then_ns <= now_ns) return 0;
  int64_t ms = (then_ns - now_ns + 999999) / 1000000;
  return ms > 0x7ffffffe ? 0x7ffffffe : static_cast<uint32_t>(ms);
}

void RegisterLibc(SymbolTable& t) {
  // Memory.
  ENTRY("malloc", Malloc);
  ENTRY("free", Free);
  ENTRY("calloc", Calloc);
  ENTRY("realloc", Realloc);
  ENTRY("posix_memalign", PosixMemalign);
  ENTRY("memalign", Memalign);
  ENTRY("aligned_alloc", Memalign);
  ENTRY("malloc_usable_size", MallocUsableSize);
  // mem/str.
  SELF(memcpy);
  SELF(memmove);
  SELF(memset);
  SELF(memcmp);
  ENTRY("memchr", Memchr);
  ENTRY("memmem", Memmem);
  SELF(strlen);
  SELF(strnlen);
  SELF(strcmp);
  SELF(strncmp);
  SELF(strcoll);
  SELF(strcpy);
  SELF(strncpy);
  SELF(strcat);
  SELF(strncat);
  ENTRY("strchr", Strchr);
  ENTRY("strrchr", Strrchr);
  ENTRY("strstr", Strstr);
  ENTRY("strpbrk", Strpbrk);
  SELF(strspn);
  SELF(strcspn);
  ENTRY("strdup", Strdup);
  ENTRY("strndup", Strndup);
  ENTRY("strlcpy", Strlcpy);
  ENTRY("strlcat", Strlcat);
  ENTRY("strtok_r", StrtokR);
  ENTRY("strerror", Strerror);
  ENTRY("strerror_r", StrerrorR);
  ENTRY("__gnu_strerror_r", Strerror);
  ENTRY("strcasecmp", Strcasecmp);
  ENTRY("strncasecmp", Strncasecmp);
  ENTRY("strtol", Strtol);
  ENTRY("strtoul", Strtoul);
  ENTRY("strtoll", Strtol);
  ENTRY("strtoull", Strtoul);
  ENTRY("strtoimax", Strtol);
  ENTRY("strtoumax", Strtoul);
  ENTRY("strtod", WRAPS(double (*)(const char*, char**), ::strtod));
  ENTRY("strtof", WRAPS(float (*)(const char*, char**), ::strtof));
  SELF(atoi);
  ENTRY("atol", Atol);
  ENTRY("atoll", Atol);
  ENTRY("atof", WRAPS(double (*)(const char*), ::atof));
  ENTRY("abs", WRAPS(int (*)(int), ::abs));
  ENTRY("labs", Labs);
  ENTRY("llabs", Labs);
  // ctype.
  SELF(isalpha);
  SELF(isdigit);
  SELF(isalnum);
  SELF(isspace);
  SELF(isupper);
  SELF(islower);
  SELF(isxdigit);
  SELF(isprint);
  SELF(ispunct);
  SELF(iscntrl);
  SELF(isgraph);
  SELF(toupper);
  SELF(tolower);
  // FORTIFY.
  ENTRY("__memcpy_chk", MemcpyChk);
  ENTRY("__memmove_chk", MemmoveChk);
  ENTRY("__memset_chk", MemsetChk);
  ENTRY("__strlen_chk", StrlenChk);
  ENTRY("__strcpy_chk", StrcpyChk);
  ENTRY("__strcat_chk", StrcatChk);
  ENTRY("__strncpy_chk", StrncpyChk);
  ENTRY("__strncpy_chk2", StrncpyChk2);
  ENTRY("__strncat_chk", StrncatChk);
  ENTRY("__strchr_chk", StrchrChk);
  ENTRY("__strrchr_chk", StrrchrChk);
  ENTRY("__strlcpy_chk", StrlcpyChk);
  ENTRY("__strlcat_chk", StrlcatChk);
  // Sorting.
  ENTRY("qsort", Qsort);
  ENTRY("bsearch", Bsearch);
  // Process and environment.
  ENTRY("__errno", Errno);
  ENTRY("abort", Abort);
  ENTRY("exit", Exit);
  ENTRY("_exit", UnderscoreExit);
  ENTRY("_Exit", UnderscoreExit);
  ENTRY("__assert2", Assert2);
  ENTRY("__assert", Assert);
  ENTRY("android_set_abort_message", SetAbortMessage);
  ENTRY("getenv", Getenv);
  ENTRY("setenv", Setenv);
  ENTRY("unsetenv", Unsetenv);
  ENTRY("getpid", Getpid);
  ENTRY("gettid", Gettid);
  ENTRY("getpagesize", Getpagesize);
  ENTRY("__system_property_get", SystemPropertyGet);
  ENTRY("rand", Rand);
  ENTRY("srand", Srandom);
  ENTRY("random", Random);
  ENTRY("srandom", Srandom);
  ENTRY("lrand48", Random);
  ENTRY("srand48", Srandom);
  ENTRY("arc4random", ArcRandom);
  ENTRY("arc4random_buf", ArcRandomBuf);
  ENTRY("arc4random_uniform", ArcRandomUniform);
  // Time.
  ENTRY("clock_gettime", ClockGettime);
  ENTRY("clock_getres", ClockGetres);
  ENTRY("gettimeofday", Gettimeofday);
  ENTRY("time", Time);
  ENTRY("nanosleep", Nanosleep);
  ENTRY("usleep", Usleep);
  ENTRY("sleep", SleepSeconds);
  ENTRY("clock", Clock);
  // fd-level files.
  ENTRY("open", Open);
  ENTRY("open64", Open);
  ENTRY("__open_2", Open);
  ENTRY("close", Close);
  ENTRY("read", Read);
  ENTRY("write", Write);
  ENTRY("pread", Pread);
  ENTRY("pread64", Pread);
  ENTRY("pwrite", Pwrite);
  ENTRY("pwrite64", Pwrite);
  ENTRY("lseek", Lseek);
  ENTRY("lseek64", Lseek);
  ENTRY("unlink", Unlink);
  ENTRY("access", Access);
  ENTRY("mkdir", Mkdir);
  ENTRY("dup", Dup);
  ENTRY("dup2", Dup2);
  ENTRY("isatty", Isatty);
  ENTRY("stat", Stat);
  ENTRY("stat64", Stat);
  ENTRY("lstat", Lstat);
  ENTRY("lstat64", Lstat);
  ENTRY("fstat", Fstat);
  ENTRY("fstat64", Fstat);
  ENTRY("mmap", Mmap);
  ENTRY("mmap64", Mmap);
  ENTRY("munmap", Munmap);
  ENTRY("mprotect", Mprotect);
  // libm (forwarded to the CRT; overloads selected by signature).
  MATH(sin); MATH(cos); MATH(tan); MATH(asin); MATH(acos); MATH(atan);
  MATH(sinh); MATH(cosh); MATH(tanh); MATH(exp); MATH(exp2); MATH(log);
  MATH(log10); MATH(log2); MATH(log1p); MATH(expm1); MATH(sqrt); MATH(cbrt);
  MATH(floor); MATH(ceil); MATH(round); MATH(trunc); MATH(fabs); MATH(rint);
  MATH(nearbyint);
  MATH2(pow); MATH2(atan2); MATH2(fmod); MATH2(hypot); MATH2(fmin); MATH2(fmax);
  MATH2(copysign); MATH2(remainder);
  MATHF(sinf); MATHF(cosf); MATHF(tanf); MATHF(asinf); MATHF(acosf); MATHF(atanf);
  MATHF(expf); MATHF(logf); MATHF(log10f); MATHF(log2f); MATHF(sqrtf); MATHF(floorf);
  MATHF(ceilf); MATHF(roundf); MATHF(truncf); MATHF(fabsf); MATHF(tanhf); MATHF(exp2f);
  MATHF(rintf); MATHF(cbrtf);
  MATH2F(powf); MATH2F(atan2f); MATH2F(fmodf); MATH2F(hypotf); MATH2F(fminf);
  MATH2F(fmaxf); MATH2F(copysignf);
  ENTRY("ldexp", Ldexp);
  ENTRY("scalbn", Ldexp);
  ENTRY("ldexpf", Ldexpf);
  ENTRY("scalbnf", Ldexpf);
  ENTRY("frexp", Frexp);
  ENTRY("modf", Modf);
  ENTRY("lround", Lround);
  ENTRY("llround", Lround);
  ENTRY("lroundf", Lroundf);
  ENTRY("llroundf", Lroundf);
  ENTRY("lrint", Lrint);
  ENTRY("llrint", Lrint);
  ENTRY("lrintf", Lrintf);
  ENTRY("llrintf", Lrintf);
  // libdl.
  ENTRY("dlopen", DlOpen);
  ENTRY("android_dlopen_ext", DlOpen);
  ENTRY("dlsym", DlSym);
  ENTRY("dlvsym", DlSym);
  ENTRY("dlclose", DlClose);
  ENTRY("dlerror", DlError);
  ENTRY("dladdr", DlAddr);
  ENTRY("dl_iterate_phdr", DlIteratePhdr);
  // C runtime support.
  ENTRY("syscall", Syscall);
  ENTRY("__stack_chk_fail", StackChkFail);
  ENTRY("__stack_chk_guard", &g_stack_chk_guard);
  ENTRY("__cxa_atexit", CxaAtexit);
  ENTRY("__cxa_finalize", CxaFinalize);
  ENTRY("atexit", Atexit);
  ENTRY("__tls_get_addr", TlsGetAddr);
}

namespace {
SymbolTable& Table() {
  static SymbolTable* t = [] {
    auto* table = new SymbolTable();
    RegisterLibc(*table);
    RegisterStdio(*table);
    RegisterPthread(*table);
    RegisterLocale(*table);
    RegisterPosix(*table);
    RegisterSocket(*table);
    RegisterSignal(*table);
    RegisterTime(*table);
    return table;
  }();
  return *t;
}
}  // namespace

void* LookupBionicSymbol(const char* name) {
  auto& table = Table();
  auto it = table.find(name);
  return it != table.end() ? it->second : nullptr;
}

void AddBionicSymbol(const char* name, void* address) { Table()[name] = address; }

void InstallBionicShim() {
  Table();
  gesher::elf::SetBionicResolver([](const char* name) { return LookupBionicSymbol(name); });
  gesher::elf::SetUnresolvedHandler(ReportUnresolved);
}

}  // namespace gesher::bionic
