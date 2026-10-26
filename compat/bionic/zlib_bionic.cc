// gesher bionic shim: libz.so, the NDK's zlib, over the zlib Gesher builds
// (external/zlib, cmake/aosp/third_party.cmake). docs/native-libs.md, ADR 0057.
//
// zlib's interface is not the same on both sides: `uLong` is 64-bit in the
// .so and 32-bit here, so z_stream's total_in/total_out/adler and every uLong
// argument differ, and the .so's zalloc/zfree are System V. Each of the .so's
// streams gets a shadow host z_stream: the .so's `state` field (zlib's
// internal pointer, opaque to callers) points at it, the public fields are
// copied in before each call and out after it, and the host stream's
// zalloc/zfree call the .so's through Microsoft-ABI trampolines. gzFile is
// opaque but for gzgetc's macro, which reads the first three fields: they are
// at the same offsets here (have, next, pos), with pos 32-bit (files past
// 2 GiB through gzgetc's macro are not supported).

#include "extra_bionic.h"

#include <gesher/bionic_shim.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <zlib.h>

extern "C" {
int __attribute__((sysv_abi)) gesher_bionic_vsnprintf(char* buf, size_t n, const char* fmt,
                                                      gesher::bionic::SysvVaList* ap);
}

namespace gesher::bionic {

namespace {

#define E(name, fn) t[name] = reinterpret_cast<void*>(&fn)

// bionic's LP64 z_stream.
struct AppZStream {
  const uint8_t* next_in;
  uint32_t avail_in;
  uint64_t total_in;
  uint8_t* next_out;
  uint32_t avail_out;
  uint64_t total_out;
  const char* msg;
  void* state;
  void* zalloc;
  void* zfree;
  void* opaque;
  int data_type;
  uint64_t adler;
  uint64_t reserved;
};
static_assert(sizeof(AppZStream) == 112, "bionic z_stream");

using AppAlloc = void*(BIONIC_SYSV*)(void* opaque, unsigned items, unsigned size);
using AppFree = void(BIONIC_SYSV*)(void* opaque, void* address);

constexpr uint32_t kMagic = 0x7a6c6962;  // "zlib"

struct Shadow {
  z_stream host;
  uint32_t magic = kMagic;
  AppAlloc zalloc = nullptr;
  AppFree zfree = nullptr;
  void* opaque = nullptr;
  uLong last_in = 0, last_out = 0;  // host totals after the last copy out
};

voidpf HostAlloc(voidpf opaque, uInt items, uInt size) {
  auto* s = static_cast<Shadow*>(opaque);
  return s->zalloc(s->opaque, items, size);
}
void HostFree(voidpf opaque, voidpf address) {
  auto* s = static_cast<Shadow*>(opaque);
  s->zfree(s->opaque, address);
}

Shadow* Get(AppZStream* a) {
  if (a == nullptr || a->state == nullptr) return nullptr;
  auto* s = static_cast<Shadow*>(a->state);
  return s->magic == kMagic ? s : nullptr;
}

void In(AppZStream* a, Shadow* s) {
  s->host.next_in = const_cast<Bytef*>(a->next_in);
  s->host.avail_in = a->avail_in;
  s->host.next_out = a->next_out;
  s->host.avail_out = a->avail_out;
  s->host.data_type = a->data_type;
}
void Out(AppZStream* a, Shadow* s) {
  a->next_in = s->host.next_in;
  a->avail_in = s->host.avail_in;
  a->next_out = s->host.next_out;
  a->avail_out = s->host.avail_out;
  // 64-bit totals: add what the 32-bit ones moved by.
  a->total_in += static_cast<uint32_t>(s->host.total_in - s->last_in);
  a->total_out += static_cast<uint32_t>(s->host.total_out - s->last_out);
  s->last_in = s->host.total_in;
  s->last_out = s->host.total_out;
  a->msg = s->host.msg;
  a->data_type = s->host.data_type;
  a->adler = s->host.adler;
}

// A new shadow for an init call, set up from the .so's stream.
Shadow* NewShadow(AppZStream* a) {
  auto* s = new Shadow();
  std::memset(&s->host, 0, sizeof(s->host));
  if (a->zalloc != nullptr) {
    s->zalloc = reinterpret_cast<AppAlloc>(a->zalloc);
    s->host.zalloc = HostAlloc;
  }
  if (a->zfree != nullptr) {
    s->zfree = reinterpret_cast<AppFree>(a->zfree);
    s->host.zfree = HostFree;
  }
  s->opaque = a->opaque;
  s->host.opaque = (a->zalloc != nullptr || a->zfree != nullptr) ? s : nullptr;
  if (a->zalloc != nullptr && a->zfree == nullptr) s->host.zfree = nullptr;
  a->total_in = a->total_out = 0;
  In(a, s);
  return s;
}
int Finish(AppZStream* a, Shadow* s, int r) {
  if (r != Z_OK) {
    a->msg = s->host.msg;
    delete s;
    a->state = nullptr;
    return r;
  }
  a->state = s;
  Out(a, s);
  return r;
}
bool BadInit(AppZStream* a, const char* version, int size) {
  return a == nullptr || version == nullptr || version[0] != ZLIB_VERSION[0] ||
         size != static_cast<int>(sizeof(AppZStream));
}

int BIONIC_SYSV InflateInit(AppZStream* a, const char* version, int size) {
  if (BadInit(a, version, size)) return a == nullptr ? Z_STREAM_ERROR : Z_VERSION_ERROR;
  Shadow* s = NewShadow(a);
  return Finish(a, s, inflateInit_(&s->host, ZLIB_VERSION, sizeof(z_stream)));
}
int BIONIC_SYSV InflateInit2(AppZStream* a, int bits, const char* version, int size) {
  if (BadInit(a, version, size)) return a == nullptr ? Z_STREAM_ERROR : Z_VERSION_ERROR;
  Shadow* s = NewShadow(a);
  return Finish(a, s, inflateInit2_(&s->host, bits, ZLIB_VERSION, sizeof(z_stream)));
}
int BIONIC_SYSV DeflateInit(AppZStream* a, int level, const char* version, int size) {
  if (BadInit(a, version, size)) return a == nullptr ? Z_STREAM_ERROR : Z_VERSION_ERROR;
  Shadow* s = NewShadow(a);
  return Finish(a, s, deflateInit_(&s->host, level, ZLIB_VERSION, sizeof(z_stream)));
}
int BIONIC_SYSV DeflateInit2(AppZStream* a, int level, int method, int bits, int mem, int strategy,
                             const char* version, int size) {
  if (BadInit(a, version, size)) return a == nullptr ? Z_STREAM_ERROR : Z_VERSION_ERROR;
  Shadow* s = NewShadow(a);
  return Finish(a, s,
                deflateInit2_(&s->host, level, method, bits, mem, strategy, ZLIB_VERSION, sizeof(z_stream)));
}

// A call on an initialized stream: copy in, call, copy out.
template <class F>
int Call(AppZStream* a, F f) {
  Shadow* s = Get(a);
  if (s == nullptr) return Z_STREAM_ERROR;
  In(a, s);
  int r = f(&s->host);
  Out(a, s);
  return r;
}
template <class F>
int End(AppZStream* a, F f) {
  Shadow* s = Get(a);
  if (s == nullptr) return Z_STREAM_ERROR;
  In(a, s);
  int r = f(&s->host);
  a->msg = s->host.msg;
  s->magic = 0;
  delete s;
  a->state = nullptr;
  return r;
}

int BIONIC_SYSV Inflate(AppZStream* a, int flush) { return Call(a, [flush](z_stream* z) { return inflate(z, flush); }); }
int BIONIC_SYSV Deflate(AppZStream* a, int flush) { return Call(a, [flush](z_stream* z) { return deflate(z, flush); }); }
int BIONIC_SYSV InflateEnd(AppZStream* a) { return End(a, [](z_stream* z) { return inflateEnd(z); }); }
int BIONIC_SYSV DeflateEnd(AppZStream* a) { return End(a, [](z_stream* z) { return deflateEnd(z); }); }
int ResetTotals(AppZStream* a, int r) {
  if (r == Z_OK) {
    Shadow* s = Get(a);
    a->total_in = a->total_out = 0;
    s->last_in = s->last_out = 0;
  }
  return r;
}
int BIONIC_SYSV InflateReset(AppZStream* a) {
  return ResetTotals(a, Call(a, [](z_stream* z) { return inflateReset(z); }));
}
int BIONIC_SYSV InflateReset2(AppZStream* a, int bits) {
  return ResetTotals(a, Call(a, [bits](z_stream* z) { return inflateReset2(z, bits); }));
}
int BIONIC_SYSV DeflateReset(AppZStream* a) {
  return ResetTotals(a, Call(a, [](z_stream* z) { return deflateReset(z); }));
}
int BIONIC_SYSV InflateSetDictionary(AppZStream* a, const Bytef* d, uInt n) {
  return Call(a, [d, n](z_stream* z) { return inflateSetDictionary(z, d, n); });
}
int BIONIC_SYSV InflateGetDictionary(AppZStream* a, Bytef* d, uInt* n) {
  return Call(a, [d, n](z_stream* z) { return inflateGetDictionary(z, d, n); });
}
int BIONIC_SYSV DeflateSetDictionary(AppZStream* a, const Bytef* d, uInt n) {
  return Call(a, [d, n](z_stream* z) { return deflateSetDictionary(z, d, n); });
}
int BIONIC_SYSV DeflateGetDictionary(AppZStream* a, Bytef* d, uInt* n) {
  return Call(a, [d, n](z_stream* z) { return deflateGetDictionary(z, d, n); });
}
int BIONIC_SYSV DeflateParams(AppZStream* a, int level, int strategy) {
  return Call(a, [level, strategy](z_stream* z) { return deflateParams(z, level, strategy); });
}
int BIONIC_SYSV DeflateTune(AppZStream* a, int gl, int ml, int nl, int mc) {
  return Call(a, [=](z_stream* z) { return deflateTune(z, gl, ml, nl, mc); });
}
int BIONIC_SYSV DeflatePrime(AppZStream* a, int bits, int value) {
  return Call(a, [bits, value](z_stream* z) { return deflatePrime(z, bits, value); });
}
int BIONIC_SYSV DeflatePending(AppZStream* a, unsigned* pending, int* bits) {
  return Call(a, [pending, bits](z_stream* z) { return deflatePending(z, pending, bits); });
}
int BIONIC_SYSV InflatePrime(AppZStream* a, int bits, int value) {
  return Call(a, [bits, value](z_stream* z) { return inflatePrime(z, bits, value); });
}
int BIONIC_SYSV InflateSync(AppZStream* a) { return Call(a, [](z_stream* z) { return inflateSync(z); }); }
int64_t BIONIC_SYSV InflateMark(AppZStream* a) {
  Shadow* s = Get(a);
  if (s == nullptr) return -(int64_t{1} << 16);
  return inflateMark(&s->host);
}
int BIONIC_SYSV InflateUndermine(AppZStream* a, int subvert) {
  return Call(a, [subvert](z_stream* z) { return inflateUndermine(z, subvert); });
}
int BIONIC_SYSV InflateValidate(AppZStream* a, int check) {
  return Call(a, [check](z_stream* z) { return inflateValidate(z, check); });
}
uint64_t BIONIC_SYSV InflateCodesUsed(AppZStream* a) {
  Shadow* s = Get(a);
  return s == nullptr ? static_cast<uint64_t>(-1) : inflateCodesUsed(&s->host);
}
uint64_t BIONIC_SYSV DeflateBound(AppZStream* a, uint64_t n) {
  Shadow* s = Get(a);
  uLong len = n > 0xffffffffULL ? 0xffffffffUL : static_cast<uLong>(n);
  if (n > 0x7fff0000ULL) return n + (n >> 12) + (n >> 14) + (n >> 25) + 13 + 18;  // zlib's bound
  return deflateBound(s != nullptr ? &s->host : nullptr, len);
}
template <class F>
int Copy(AppZStream* dest, AppZStream* source, F f) {
  Shadow* src = Get(source);
  if (dest == nullptr || src == nullptr) return Z_STREAM_ERROR;
  In(source, src);
  std::memcpy(dest, source, sizeof(*dest));
  auto* s = new Shadow();
  std::memcpy(&s->host, &src->host, sizeof(s->host));
  s->zalloc = src->zalloc;
  s->zfree = src->zfree;
  s->opaque = src->opaque;
  s->last_in = src->last_in;
  s->last_out = src->last_out;
  if (s->host.opaque == src) s->host.opaque = s;
  int r = f(&s->host, &src->host);
  // zlib's copy took the source's z_stream, opaque included: the copy's own.
  if (s->host.opaque == src) s->host.opaque = s;
  if (r != Z_OK) {
    delete s;
    dest->state = nullptr;
    return r;
  }
  dest->state = s;
  return r;
}
int BIONIC_SYSV InflateCopy(AppZStream* dest, AppZStream* source) {
  return Copy(dest, source, [](z_stream* d, z_stream* s) { return inflateCopy(d, s); });
}
int BIONIC_SYSV DeflateCopy(AppZStream* dest, AppZStream* source) {
  return Copy(dest, source, [](z_stream* d, z_stream* s) { return deflateCopy(d, s); });
}

// --- the utility functions ----------------------------------------------------------

bool Fits(uint64_t n) { return n <= 0xffffffffULL; }
int BIONIC_SYSV Compress2(Bytef* dest, uint64_t* dest_len, const Bytef* src, uint64_t src_len, int level) {
  if (!Fits(*dest_len) || !Fits(src_len)) return Z_MEM_ERROR;
  uLongf d = static_cast<uLongf>(*dest_len);
  int r = compress2(dest, &d, src, static_cast<uLong>(src_len), level);
  *dest_len = d;
  return r;
}
int BIONIC_SYSV Compress(Bytef* dest, uint64_t* dest_len, const Bytef* src, uint64_t src_len) {
  return Compress2(dest, dest_len, src, src_len, Z_DEFAULT_COMPRESSION);
}
uint64_t BIONIC_SYSV CompressBound(uint64_t n) {
  return n + (n >> 12) + (n >> 14) + (n >> 25) + 13;
}
int BIONIC_SYSV Uncompress2(Bytef* dest, uint64_t* dest_len, const Bytef* src, uint64_t* src_len) {
  if (!Fits(*dest_len) || !Fits(*src_len)) return Z_MEM_ERROR;
  uLongf d = static_cast<uLongf>(*dest_len);
  uLong s = static_cast<uLong>(*src_len);
  int r = uncompress2(dest, &d, src, &s);
  *dest_len = d;
  *src_len = s;
  return r;
}
int BIONIC_SYSV Uncompress(Bytef* dest, uint64_t* dest_len, const Bytef* src, uint64_t src_len) {
  return Uncompress2(dest, dest_len, src, &src_len);
}
uint64_t BIONIC_SYSV Crc32(uint64_t crc, const Bytef* buf, uInt len) {
  return crc32(static_cast<uLong>(crc), buf, len);
}
uint64_t BIONIC_SYSV Crc32Z(uint64_t crc, const Bytef* buf, size_t len) {
  uLong c = static_cast<uLong>(crc);
  while (len > 0) {
    uInt n = len > 0x40000000 ? 0x40000000u : static_cast<uInt>(len);
    c = crc32(c, buf, n);
    buf += n;
    len -= n;
  }
  return c;
}
uint64_t BIONIC_SYSV Crc32Combine(uint64_t a, uint64_t b, int64_t len) {
  return crc32_combine(static_cast<uLong>(a), static_cast<uLong>(b), static_cast<z_off_t>(len));
}
uint64_t BIONIC_SYSV Adler32(uint64_t adler, const Bytef* buf, uInt len) {
  return adler32(static_cast<uLong>(adler), buf, len);
}
uint64_t BIONIC_SYSV Adler32Z(uint64_t adler, const Bytef* buf, size_t len) {
  uLong c = static_cast<uLong>(adler);
  while (len > 0) {
    uInt n = len > 0x40000000 ? 0x40000000u : static_cast<uInt>(len);
    c = adler32(c, buf, n);
    buf += n;
    len -= n;
  }
  return c;
}
uint64_t BIONIC_SYSV Adler32Combine(uint64_t a, uint64_t b, int64_t len) {
  return adler32_combine(static_cast<uLong>(a), static_cast<uLong>(b), static_cast<z_off_t>(len));
}
const char* BIONIC_SYSV ZlibVersion() { return zlibVersion(); }
uint64_t BIONIC_SYSV ZlibCompileFlags() {
  // Bits 0-7 are the sizes of uInt, uLong, voidpf and z_off_t as the .so
  // sees them (2 = 64-bit): uLong and z_off_t are 64-bit there.
  return (zlibCompileFlags() & ~0xffUL) | 0 | (2 << 2) | (2 << 4) | (2 << 6);
}
const char* BIONIC_SYSV ZError(int e) { return zError(e); }
const void* BIONIC_SYSV GetCrcTable() { return get_crc_table(); }

// --- gz* -------------------------------------------------------------------------------
// The file is opened with gesher-base's open (Linux paths), then gzdopen'd.

gzFile GzOpenFd(int fd, const char* mode) {
  std::string m;
  for (const char* p = mode; *p; ++p) {
    if (*p != 'e' && *p != 'x') m += *p;  // O_CLOEXEC/O_EXCL are in the open flags
  }
  gzFile f = gzdopen(fd, m.c_str());
  if (f == nullptr) close(fd);
  return f;
}
gzFile BIONIC_SYSV GzOpen(const char* path, const char* mode) {
  if (path == nullptr || mode == nullptr) return nullptr;
  int flags = -1;
  bool excl = false, cloexec = false;
  for (const char* p = mode; *p; ++p) {
    if (*p == 'r') flags = O_RDONLY;
    if (*p == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (*p == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    if (*p == 'x') excl = true;
    if (*p == 'e') cloexec = true;
  }
  if (flags < 0) return errno = kEINVAL, nullptr;
  if (excl && (flags & O_CREAT)) flags |= O_EXCL;
  if (cloexec) flags |= O_CLOEXEC;
  int fd = open(path, flags, 0666);
  if (fd < 0) return nullptr;
  return GzOpenFd(fd, mode);
}
gzFile BIONIC_SYSV GzDopen(int fd, const char* mode) {
  if (fd < 0 || mode == nullptr) return nullptr;
  std::string m;
  for (const char* p = mode; *p; ++p) {
    if (*p != 'e' && *p != 'x') m += *p;
  }
  return gzdopen(fd, m.c_str());
}
int BIONIC_SYSV GzBuffer(gzFile f, unsigned size) { return gzbuffer(f, size); }
int BIONIC_SYSV GzSetparams(gzFile f, int level, int strategy) { return gzsetparams(f, level, strategy); }
int BIONIC_SYSV GzRead(gzFile f, voidp buf, unsigned len) { return gzread(f, buf, len); }
size_t BIONIC_SYSV GzFread(voidp buf, size_t size, size_t n, gzFile f) {
  return gzfread(buf, static_cast<z_size_t>(size), static_cast<z_size_t>(n), f);
}
int BIONIC_SYSV GzWrite(gzFile f, voidpc buf, unsigned len) { return gzwrite(f, buf, len); }
size_t BIONIC_SYSV GzFwrite(voidpc buf, size_t size, size_t n, gzFile f) {
  return gzfwrite(buf, static_cast<z_size_t>(size), static_cast<z_size_t>(n), f);
}
int BIONIC_SYSV GzPuts(gzFile f, const char* s) { return gzputs(f, s); }
char* BIONIC_SYSV GzGets(gzFile f, char* buf, int len) { return gzgets(f, buf, len); }
int BIONIC_SYSV GzPutc(gzFile f, int c) { return gzputc(f, c); }
int BIONIC_SYSV GzGetc(gzFile f) { return gzgetc(f); }
int BIONIC_SYSV GzUngetc(int c, gzFile f) { return gzungetc(c, f); }
int BIONIC_SYSV GzFlush(gzFile f, int flush) { return gzflush(f, flush); }
int64_t BIONIC_SYSV GzSeek(gzFile f, int64_t off, int whence) {
  return gzseek(f, static_cast<z_off_t>(off), whence);
}
int BIONIC_SYSV GzRewind(gzFile f) { return gzrewind(f); }
int64_t BIONIC_SYSV GzTell(gzFile f) { return gztell(f); }
int64_t BIONIC_SYSV GzOffset(gzFile f) { return gzoffset(f); }
int BIONIC_SYSV GzEof(gzFile f) { return gzeof(f); }
int BIONIC_SYSV GzDirect(gzFile f) { return gzdirect(f); }
int BIONIC_SYSV GzClose(gzFile f) { return gzclose(f); }
int BIONIC_SYSV GzCloseR(gzFile f) { return gzclose_r(f); }
int BIONIC_SYSV GzCloseW(gzFile f) { return gzclose_w(f); }
const char* BIONIC_SYSV GzError(gzFile f, int* errnum) { return gzerror(f, errnum); }
void BIONIC_SYSV GzClearerr(gzFile f) { gzclearerr(f); }

}  // namespace

extern "C" int BIONIC_SYSV gesher_zlib_gzvprintf(gzFile f, const char* fmt, SysvVaList* ap) {
  SysvVaList copy = *ap;
  char small[1024];
  int n = gesher_bionic_vsnprintf(small, sizeof(small), fmt, ap);
  if (n < 0) return Z_STREAM_ERROR;
  if (static_cast<size_t>(n) < sizeof(small)) return n == 0 ? 0 : gzwrite(f, small, static_cast<unsigned>(n));
  std::string big(static_cast<size_t>(n) + 1, '\0');
  gesher_bionic_vsnprintf(big.data(), big.size(), fmt, &copy);
  return gzwrite(f, big.data(), static_cast<unsigned>(n));
}

asm(".text\n" GESHER_VA_TRAMPOLINE("gesher_zlib_gzprintf", "gesher_zlib_gzvprintf", "16", "%rdx"));
extern "C" int gesher_zlib_gzprintf();

void RegisterZlib(SymbolTable& t) {
  E("zlibVersion", ZlibVersion);
  E("zlibCompileFlags", ZlibCompileFlags);
  E("zError", ZError);
  E("get_crc_table", GetCrcTable);
  E("inflateInit_", InflateInit);
  E("inflateInit2_", InflateInit2);
  E("deflateInit_", DeflateInit);
  E("deflateInit2_", DeflateInit2);
  E("inflate", Inflate);
  E("deflate", Deflate);
  E("inflateEnd", InflateEnd);
  E("deflateEnd", DeflateEnd);
  E("inflateReset", InflateReset);
  E("inflateReset2", InflateReset2);
  E("deflateReset", DeflateReset);
  E("inflateSetDictionary", InflateSetDictionary);
  E("inflateGetDictionary", InflateGetDictionary);
  E("deflateSetDictionary", DeflateSetDictionary);
  E("deflateGetDictionary", DeflateGetDictionary);
  E("deflateParams", DeflateParams);
  E("deflateTune", DeflateTune);
  E("deflatePrime", DeflatePrime);
  E("deflatePending", DeflatePending);
  E("deflateBound", DeflateBound);
  E("deflateCopy", DeflateCopy);
  E("inflatePrime", InflatePrime);
  E("inflateSync", InflateSync);
  E("inflateMark", InflateMark);
  E("inflateUndermine", InflateUndermine);
  E("inflateValidate", InflateValidate);
  E("inflateCodesUsed", InflateCodesUsed);
  E("inflateCopy", InflateCopy);
  E("compress", Compress);
  E("compress2", Compress2);
  E("compressBound", CompressBound);
  E("uncompress", Uncompress);
  E("uncompress2", Uncompress2);
  E("crc32", Crc32);
  E("crc32_z", Crc32Z);
  E("crc32_combine", Crc32Combine);
  E("crc32_combine64", Crc32Combine);
  E("adler32", Adler32);
  E("adler32_z", Adler32Z);
  E("adler32_combine", Adler32Combine);
  E("adler32_combine64", Adler32Combine);
  E("gzopen", GzOpen);
  E("gzopen64", GzOpen);
  E("gzdopen", GzDopen);
  E("gzbuffer", GzBuffer);
  E("gzsetparams", GzSetparams);
  E("gzread", GzRead);
  E("gzfread", GzFread);
  E("gzwrite", GzWrite);
  E("gzfwrite", GzFwrite);
  E("gzputs", GzPuts);
  E("gzgets", GzGets);
  E("gzputc", GzPutc);
  E("gzgetc", GzGetc);
  E("gzgetc_", GzGetc);
  E("gzungetc", GzUngetc);
  E("gzflush", GzFlush);
  E("gzseek", GzSeek);
  E("gzseek64", GzSeek);
  E("gzrewind", GzRewind);
  E("gztell", GzTell);
  E("gztell64", GzTell);
  E("gzoffset", GzOffset);
  E("gzoffset64", GzOffset);
  E("gzeof", GzEof);
  E("gzdirect", GzDirect);
  E("gzclose", GzClose);
  E("gzclose_r", GzCloseR);
  E("gzclose_w", GzCloseW);
  E("gzerror", GzError);
  E("gzclearerr", GzClearerr);
  t["gzvprintf"] = reinterpret_cast<void*>(&gesher_zlib_gzvprintf);
  t["gzprintf"] = reinterpret_cast<void*>(&gesher_zlib_gzprintf);
}

}  // namespace gesher::bionic
