// native2 track test library (docs/native-libs.md, ADR 0057): built by the
// NDK for x86_64 and arm64-v8a (.github/workflows/native2.yml) and loaded by
// dalvikvm (gesher.Native2Test) and by the native2app APK through gesher-app.
//
// run(dir, symbols) checks, in this order, and returns one line per part:
//   extras:   functions the shim gained (semaphores, wide characters, libm,
//             fenv, div, rusage, strptime, getopt_long, pwd, properties,
//             barriers, spinlocks, sysinfo, prctl, getrandom, zlib);
//   dlopen:   the platform's libraries by name and by /system/lib64 path,
//             dlsym on their handles and RTLD_DEFAULT, calls through them, an
//             app library by bare name, libc++_shared already loaded;
//   files:    flock, the execute bit, symbolic links, inotify IN_MODIFY
//             before close;
//   symbols:  every symbol named in the `symbols` file (lines
//             "<set> <library> <symbol> <bind>") through dlsym(RTLD_DEFAULT).
//             Executable-only __libc_init (LAUNCHER) is reported separately;
//             gesher-exec's real program runs exercise that startup entry.
// The last line is "native2: all passed" when every check of the first three
// parts held.

#include <jni.h>

#include <EGL/egl.h>
#include <SLES/OpenSLES.h>
#include <aaudio/AAudio.h>
#include <android/api-level.h>
#include <android/bitmap.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fenv.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../native/arm64_fpcr_contracts.h"

namespace {

struct Part {
  std::string name;
  std::vector<std::string> failures;
  std::vector<std::string> notes;
  void Check(bool ok, const std::string& what) {
    if (!ok) failures.push_back(what + (errno != 0 ? " (errno " + std::to_string(errno) + ")" : ""));
    errno = 0;
  }
  std::string Line() const {
    std::string s = "native2 " + name + ": ";
    if (failures.empty()) {
      s += "ok";
    } else {
      s += "FAILED:";
      for (const std::string& f : failures) s += " [" + f + "]";
    }
    for (const std::string& n : notes) s += "; " + n;
    return s;
  }
};

bool Near(double a, double b, double eps = 1e-9) { return fabs(a - b) <= eps * (1 + fabs(b)); }

// --- extras ----------------------------------------------------------------------

sem_t g_sem;
std::atomic<int> g_sem_taken{0};

void* SemWorker(void*) {
  for (int i = 0; i < 100; ++i) {
    sem_wait(&g_sem);
    g_sem_taken++;
  }
  return nullptr;
}

pthread_barrier_t g_barrier;
std::atomic<int> g_serial{0}, g_after{0};
pthread_spinlock_t g_spin;
int g_spin_counter = 0;

void* BarrierWorker(void*) {
  for (int i = 0; i < 1000; ++i) {
    pthread_spin_lock(&g_spin);
    g_spin_counter++;
    pthread_spin_unlock(&g_spin);
  }
  if (pthread_barrier_wait(&g_barrier) == PTHREAD_BARRIER_SERIAL_THREAD) g_serial++;
  g_after++;
  return nullptr;
}

void* ZAlloc(void* opaque, unsigned items, unsigned size) {
  ++*static_cast<int*>(opaque);
  return calloc(items, size);
}
void ZFree(void*, void* p) { free(p); }

Part Extras(const std::string& dir) {
  Part p{"extras"};
  errno = 0;
  // Semaphores across threads; a timed wait that times out.
  p.Check(sem_init(&g_sem, 0, 0) == 0, "sem_init");
  pthread_t t;
  pthread_create(&t, nullptr, SemWorker, nullptr);
  for (int i = 0; i < 100; ++i) sem_post(&g_sem);
  pthread_join(t, nullptr);
  p.Check(g_sem_taken == 100, "sem_post/sem_wait across threads");
  int v = -1;
  p.Check(sem_getvalue(&g_sem, &v) == 0 && v == 0, "sem_getvalue");
  timespec abs;
  clock_gettime(CLOCK_REALTIME, &abs);
  abs.tv_nsec += 50 * 1000000;
  if (abs.tv_nsec >= 1000000000) abs.tv_sec++, abs.tv_nsec -= 1000000000;
  p.Check(sem_timedwait(&g_sem, &abs) == -1 && errno == ETIMEDOUT, "sem_timedwait times out");
  errno = 0;
  p.Check(sem_trywait(&g_sem) == -1 && errno == EAGAIN, "sem_trywait EAGAIN");
  errno = 0;
  sem_destroy(&g_sem);

  // Wide characters (32-bit wchar_t).
  const wchar_t* hello = L"Hello, שלום";
  p.Check(wcscmp(hello, L"Hello, שלום") == 0, "wcscmp");
  p.Check(wcsncmp(hello, L"Help", 3) == 0 && wcsncmp(hello, L"Help", 4) < 0, "wcsncmp");
  p.Check(wcsstr(hello, L"לו") == hello + 8, "wcsstr");
  p.Check(wcscasecmp(L"ABC", L"abc") == 0, "wcscasecmp");
  p.Check(towupper(L'a') == L'A' && towlower(L'Α') == L'α' &&
              towupper(0x10428) == 0x10400 && towlower(0x10400) == 0x10428 &&
              towlower(WEOF) == WEOF, "towupper/towlower (Greek, supplementary, WEOF)");
  p.Check(iswalpha(L'ש') && !iswdigit(L'x') && iswspace(L' '), "iswalpha/iswdigit/iswspace");
  p.Check(iswalpha(0x10400) && !iswalpha(0x1f600) && iswprint(0x1f600) &&
              !iswprint(0x10ffff) && !iswalpha(WEOF), "Unicode supplementary classes");
  p.Check(wcwidth(L'一') == 2 && wcwidth(L'a') == 1, "wcwidth");
  wchar_t buf[32];
  wcslcpy(buf, L"abcdef", 4);
  p.Check(wcscmp(buf, L"abc") == 0, "wcslcpy");
  wmemset(buf, L'z', 3);
  buf[3] = 0;
  p.Check(wcscmp(buf, L"zzz") == 0, "wmemset");
  wcscpy(buf, L"ab");
  wcscat(buf, L"cd");
  p.Check(wcscmp(buf, L"abcd") == 0 && wcschr(buf, L'c') == buf + 2 && wcsrchr(buf, L'a') == buf,
          "wcscpy/wcscat/wcschr");
  char mb[32];
  p.Check(wcstombs(mb, L"שx", sizeof(mb)) == 3 && strcmp(mb, "\xd7\xa9x") == 0, "wcstombs");
  p.Check(mbstowcs(buf, "\xd7\xa9y", 32) == 2 && buf[0] == 0x5e9, "mbstowcs");
  p.notes.push_back("Unicode case/classes (BMP and supplementary)");

  int sockets[2] = {-1, -1};
  p.Check(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0, "datagram socketpair");
  if (sockets[0] >= 0 && sockets[1] >= 0) {
    char first[] = "one", second[] = "two";
    iovec tx_iov[] = {{first, 3}, {second, 3}};
    mmsghdr tx[2]{};
    for (int i = 0; i < 2; ++i) {
      tx[i].msg_hdr.msg_iov = &tx_iov[i];
      tx[i].msg_hdr.msg_iovlen = 1;
    }
    p.Check(sendmmsg(sockets[0], tx, 2, 0) == 2 && tx[0].msg_len == 3 && tx[1].msg_len == 3,
            "sendmmsg two datagrams");
    char data[2][16]{};
    iovec rx_iov[] = {{data[0], sizeof(data[0])}, {data[1], sizeof(data[1])}};
    mmsghdr rx[2]{};
    for (int i = 0; i < 2; ++i) {
      rx[i].msg_hdr.msg_iov = &rx_iov[i];
      rx[i].msg_hdr.msg_iovlen = 1;
    }
    timespec timeout{2, 0};
    p.Check(recvmmsg(sockets[1], rx, 2, 0, &timeout) == 2 &&
                rx[0].msg_len == 3 && rx[1].msg_len == 3 &&
                memcmp(data[0], "one", 3) == 0 && memcmp(data[1], "two", 3) == 0,
            "recvmmsg two datagrams");
    p.Check(send(sockets[0], first, 3, 0) == 3 &&
                recvmmsg(sockets[1], rx, 2, MSG_WAITFORONE, nullptr) == 1,
            "recvmmsg MSG_WAITFORONE returns partial batch");
    errno = 0;
    p.Check(recvmmsg(sockets[1], rx, 2, MSG_DONTWAIT, nullptr) == -1 && errno == EAGAIN,
            "recvmmsg MSG_DONTWAIT empty");
    timeout = {0, 20000000};
    p.Check(recvmmsg(sockets[1], rx, 2, 0, &timeout) == 0 &&
                timeout.tv_sec == 0 && timeout.tv_nsec == 0, "recvmmsg timeout");
    timeout = {0, 1000000000};
    p.Check(recvmmsg(sockets[1], rx, 2, 0, &timeout) == -1 && errno == EINVAL,
            "recvmmsg invalid timeout");
    close(sockets[0]);
    close(sockets[1]);
  }
  alignas(cmsghdr) char control[2 * CMSG_SPACE(sizeof(int))]{};
  msghdr msg{};
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  auto* next = reinterpret_cast<cmsghdr*>(control + CMSG_SPACE(sizeof(int)));
  next->cmsg_len = CMSG_LEN(sizeof(int));
  p.Check(CMSG_NXTHDR(&msg, cmsg) == next && CMSG_NXTHDR(&msg, next) == nullptr,
          "CMSG_NXTHDR two headers and end");
  cmsg->cmsg_len = SIZE_MAX;
  p.Check(CMSG_NXTHDR(&msg, cmsg) == nullptr, "CMSG_NXTHDR rejects overflowing length");
  p.notes.push_back("datagram batches, timeout and ancillary header bounds");

  size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  void* memory = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  p.Check(memory != MAP_FAILED, "mmap for mremap");
  if (memory != MAP_FAILED) {
    memset(memory, 0x5a, page);
    void* larger = mremap(memory, page, page * 2, MREMAP_MAYMOVE);
    p.Check(larger != MAP_FAILED, "mremap grows anonymous mapping");
    if (larger != MAP_FAILED) {
      auto* bytes = static_cast<unsigned char*>(larger);
      p.Check(bytes[0] == 0x5a && bytes[page - 1] == 0x5a && bytes[page] == 0,
              "mremap preserves contents and zeroes new pages");
      void* smaller = mremap(larger, page * 2, page, 0);
      p.Check(smaller == larger && bytes[0] == 0x5a, "mremap shrinks in place");
      munmap(larger, smaller == MAP_FAILED ? page * 2 : page);
    } else {
      munmap(memory, page);
    }
  }
  p.notes.push_back("mremap growth, copy and shrink");

  // libm.
  p.Check(Near(erf(0.5), 0.5204998778130465), "erf");
  p.Check(Near(lgamma(5.0), log(24.0)), "lgamma");
  p.Check(Near(tgamma(5.0), 24.0), "tgamma");
  p.Check(acosh(1.0) == 0.0 && Near(asinh(1.0), 0.881373587019543), "acosh/asinh");
  p.Check(nextafter(1.0, 2.0) > 1.0 && fma(2.0, 3.0, 1.0) == 7.0, "nextafter/fma");
  p.Check(Near(erff(0.5f), 0.5205f, 1e-4) && fabsf(tgammaf(4.0f) - 6.0f) < 1e-4f, "erff/tgammaf");
  p.Check(isnan(nan("")) && ilogb(8.0) == 3, "nan/ilogb");
#if defined(__x86_64__)
  p.Check(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD, "fesetround");
  volatile double half = 0.5;
  p.Check(rint(half) == 1.0, "rint rounds up under FE_UPWARD");
  fesetround(FE_TONEAREST);
  feclearexcept(FE_ALL_EXCEPT);
  volatile double zero = 0.0;
  volatile double r = 1.0 / zero;
  (void)r;
  p.Check(fetestexcept(FE_DIVBYZERO) != 0, "fetestexcept(FE_DIVBYZERO)");
  fenv_t env;
  p.Check(fegetenv(&env) == 0 && fesetenv(FE_DFL_ENV) == 0 && fetestexcept(FE_ALL_EXCEPT) == 0,
          "fegetenv/fesetenv(FE_DFL_ENV)");
#else
  p.Check(fegetround() == FE_TONEAREST, "fegetround");
#endif

  // stdlib.
  div_t d = div(17, 5);
  ldiv_t ld = ldiv(-17000000000L, 5);
  p.Check(d.quot == 3 && d.rem == 2 && ld.quot == -3400000000L && ld.rem == 0, "div/ldiv");
  rusage ru{};
  timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  volatile uint64_t spin = 0;
  for (;;) {
    for (int i = 0; i < 1000000; ++i) spin += i;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 > 120) break;
  }
  p.Check(getrusage(RUSAGE_SELF, &ru) == 0 && (ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0) &&
              ru.ru_maxrss > 0,
          "getrusage");
  tm when{};
  p.Check(strptime("2026-10-26 13:45:07", "%Y-%m-%d %H:%M:%S", &when) != nullptr && when.tm_year == 126 &&
              when.tm_mon == 9 && when.tm_mday == 26 && when.tm_hour == 13 && when.tm_sec == 7 &&
              when.tm_wday == 1 && when.tm_yday == 298,
          "strptime");
  {
    const char* argv0[] = {"prog", "-v", "file1", "--level=3", "-o", "out", "--", "-x", nullptr};
    char* argv[9];
    for (int i = 0; i < 9; ++i) argv[i] = const_cast<char*>(argv0[i]);
    static option longopts[] = {{"level", required_argument, nullptr, 'l'}, {nullptr, 0, nullptr, 0}};
    optind = 1;
    int c, verbose = 0, level = 0;
    std::string out;
    while ((c = getopt_long(8, argv, "vo:", longopts, nullptr)) != -1) {
      if (c == 'v') verbose = 1;
      if (c == 'l') level = atoi(optarg);
      if (c == 'o') out = optarg;
    }
    p.Check(verbose == 1 && level == 3 && out == "out" && optind == 6 && strcmp(argv[6], "file1") == 0 &&
                strcmp(argv[7], "-x") == 0,
            "getopt_long (permuted operands, --)");
  }
  passwd* pw = getpwuid(getuid());
  p.Check(pw != nullptr && pw->pw_name != nullptr && pw->pw_uid == getuid(), "getpwuid");
  char prop[PROP_VALUE_MAX] = {};
  __system_property_get("ro.build.version.sdk", prop);
  p.Check(atoi(prop) >= 30 && android_get_device_api_level() == atoi(prop),
          std::string("ro.build.version.sdk=") + prop);
  p.notes.push_back(std::string("sdk ") + prop);
  const prop_info* pi = __system_property_find("ro.build.version.sdk");
  std::string cb_value;
  if (pi != nullptr) {
    __system_property_read_callback(
        pi, [](void* cookie, const char*, const char* value, uint32_t) { *static_cast<std::string*>(cookie) = value; },
        &cb_value);
  }
  p.Check(cb_value == prop, "__system_property_read_callback");

  // pthread barriers and spinlocks.
  p.Check(pthread_barrier_init(&g_barrier, nullptr, 4) == 0 && pthread_spin_init(&g_spin, 0) == 0,
          "pthread_barrier_init/pthread_spin_init");
  pthread_t ts[4];
  for (pthread_t& th : ts) pthread_create(&th, nullptr, BarrierWorker, nullptr);
  for (pthread_t& th : ts) pthread_join(th, nullptr);
  p.Check(g_serial == 1 && g_after == 4 && g_spin_counter == 4000, "pthread_barrier_wait/spinlock");
  pthread_barrier_destroy(&g_barrier);

  struct sysinfo si{};
  p.Check(sysinfo(&si) == 0 && si.totalram > 0 && si.uptime > 0, "sysinfo");
  char name[16] = {};
  p.Check(prctl(PR_SET_NAME, "native2-test") == 0 && prctl(PR_GET_NAME, name) == 0 &&
              strcmp(name, "native2-test") == 0,
          "prctl(PR_SET_NAME/PR_GET_NAME)");
  unsigned char rnd[32] = {};
  p.Check(getrandom(rnd, sizeof(rnd), 0) == 32, "getrandom");
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  p.Check(sched_getaffinity(0, sizeof(cpus), &cpus) == 0 && CPU_COUNT(&cpus) >= 1, "sched_getaffinity");

  // zlib (libz.so).
  const char* text = "Gesher's zlib: Gesher's zlib: Gesher's zlib: hello hello hello";
  uLongf clen = compressBound(strlen(text));
  std::vector<Bytef> comp(clen);
  p.Check(compress(comp.data(), &clen, reinterpret_cast<const Bytef*>(text), strlen(text)) == Z_OK, "compress");
  char back[128] = {};
  uLongf blen = sizeof(back);
  p.Check(uncompress(reinterpret_cast<Bytef*>(back), &blen, comp.data(), clen) == Z_OK && blen == strlen(text) &&
              memcmp(back, text, blen) == 0,
          "uncompress");
  p.Check(crc32(0, reinterpret_cast<const Bytef*>("123456789"), 9) == 0xCBF43926UL, "crc32");
  p.Check(adler32(1, reinterpret_cast<const Bytef*>("Wikipedia"), 9) == 0x11E60398UL, "adler32");
  {
    // Streaming, with the library's own allocator and many small steps.
    std::string big;
    for (int i = 0; i < 20000; ++i) big += "line " + std::to_string(i % 97) + "\n";
    int allocs = 0;
    z_stream zs{};
    zs.zalloc = ZAlloc;
    zs.zfree = ZFree;
    zs.opaque = &allocs;
    bool ok = deflateInit2(&zs, 9, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK;  // gzip
    std::vector<Bytef> gz(big.size() + 1024);
    zs.next_in = reinterpret_cast<Bytef*>(big.data());
    zs.avail_in = static_cast<uInt>(big.size());
    zs.next_out = gz.data();
    int r;
    do {
      zs.avail_out = 100;
      r = deflate(&zs, Z_FINISH);
    } while (r == Z_OK);
    ok = ok && r == Z_STREAM_END && zs.total_in == big.size();
    size_t gzlen = zs.total_out;
    deflateEnd(&zs);
    z_stream is{};
    ok = ok && inflateInit2(&is, 47) == Z_OK;
    std::string out(big.size(), '\0');
    is.next_in = gz.data();
    is.next_out = reinterpret_cast<Bytef*>(out.data());
    is.avail_out = static_cast<uInt>(out.size());
    size_t fed = 0;
    do {
      size_t n = gzlen - fed < 37 ? gzlen - fed : 37;
      is.next_in = gz.data() + fed;
      is.avail_in = static_cast<uInt>(n);
      r = inflate(&is, Z_NO_FLUSH);
      fed += n - is.avail_in;
    } while (r == Z_OK && fed < gzlen);
    ok = ok && r == Z_STREAM_END && is.total_out == big.size() && out == big;
    inflateEnd(&is);
    p.Check(ok && allocs > 0, "deflate/inflate streaming (gzip, custom zalloc)");
  }
  {
    std::string path = dir + "/native2.gz";
    gzFile g = gzopen(path.c_str(), "wb");
    bool ok = g != nullptr && gzprintf(g, "number %d, %s\n", 42, "text") > 0 && gzputs(g, "second\n") > 0 &&
              gzclose(g) == Z_OK;
    char line[64] = {};
    g = gzopen(path.c_str(), "rb");
    ok = ok && g != nullptr && gzgets(g, line, sizeof(line)) != nullptr &&
         strcmp(line, "number 42, text\n") == 0 && gzgetc(g) == 's';
    if (g != nullptr) gzclose(g);
    unlink(path.c_str());
    p.Check(ok, "gzopen/gzprintf/gzgets/gzgetc");
  }
  p.notes.push_back(std::string("zlib ") + zlibVersion());
  return p;
}

// --- dlopen -----------------------------------------------------------------------

#if defined(__aarch64__) || defined(__x86_64__)
const char* kSystemLib = "/system/lib64/";
#else
const char* kSystemLib = "/system/lib/";
#endif

Part Dlopen() {
  Part p{"dlopen"};
  errno = 0;
  struct Lib {
    const char* name;
    const char* probe;
    bool required;
  };
  const Lib libs[] = {
      {"libc.so", "malloc", true},          {"libm.so", "cos", true},
      {"libdl.so", "dladdr", true},         {"liblog.so", "__android_log_write", true},
      {"libz.so", "inflate", true},         {"libandroid.so", "AAssetManager_open", true},
      {"libnativewindow.so", "ANativeWindow_acquire", true}, {"libEGL.so", "eglGetDisplay", true},
      {"libGLESv1_CM.so", "glClear", true}, {"libGLESv2.so", "glDrawArrays", true},
      {"libGLESv3.so", "glDrawArraysInstanced", true}, {"libOpenSLES.so", "slCreateEngine", true},
      {"libaaudio.so", "AAudio_createStreamBuilder", true}, {"libjnigraphics.so", "AndroidBitmap_getInfo", true},
      {"libmediandk.so", "AMediaCodec_createDecoderByType", false},
      {"libcamera2ndk.so", "ACameraManager_create", false}, {"libvulkan.so", "vkCreateInstance", false},
  };
  std::string present, absent;
  for (const Lib& l : libs) {
    void* h = dlopen(l.name, RTLD_NOW);
    std::string full = std::string(kSystemLib) + l.name;
    void* h2 = dlopen(full.c_str(), RTLD_NOW);
    if (h == nullptr) {
      p.Check(!l.required, std::string("dlopen(") + l.name + "): " + (dlerror() ?: ""));
      p.Check(h2 == nullptr, std::string("dlopen(") + full + ") without the name");
      absent += std::string(absent.empty() ? "" : ",") + l.name;
      continue;
    }
    present += std::string(present.empty() ? "" : ",") + l.name;
    void* s = dlsym(h, l.probe);
    p.Check(s != nullptr, std::string("dlsym(") + l.name + ", " + l.probe + ")");
    p.Check(h2 != nullptr && dlsym(h2, l.probe) == s, std::string("dlopen/dlsym by ") + full);
    p.Check(dlsym(RTLD_DEFAULT, l.probe) == s, std::string("dlsym(RTLD_DEFAULT, ") + l.probe + ")");
    p.Check(dlclose(h) == 0, std::string("dlclose(") + l.name + ")");
    if (h2 != nullptr) dlclose(h2);
  }
  p.notes.push_back("present " + present);
  p.notes.push_back("absent " + absent);

  // Calls through the handles.
  if (void* z = dlopen("libz.so", RTLD_NOW)) {
    auto version = reinterpret_cast<const char* (*)()>(dlsym(z, "zlibVersion"));
    p.Check(version != nullptr && version()[0] == '1', "zlibVersion through dlopen(libz.so)");
  }
  if (void* egl = dlopen("libEGL.so", RTLD_NOW)) {
    auto get_display = reinterpret_cast<EGLDisplay (*)(EGLNativeDisplayType)>(dlsym(egl, "eglGetDisplay"));
    p.Check(get_display != nullptr && get_display(EGL_DEFAULT_DISPLAY) != EGL_NO_DISPLAY,
            "eglGetDisplay through dlopen(libEGL.so)");
    auto get_proc = reinterpret_cast<void* (*)(const char*)>(dlsym(egl, "eglGetProcAddress"));
    p.Check(get_proc != nullptr && get_proc("glClear") != nullptr, "eglGetProcAddress(glClear)");
  }
  if (void* sl = dlopen("libOpenSLES.so", RTLD_NOW)) {
    auto create = reinterpret_cast<SLresult (*)(SLObjectItf*, SLuint32, const SLEngineOption*, SLuint32,
                                                const SLInterfaceID*, const SLboolean*)>(dlsym(sl, "slCreateEngine"));
    SLObjectItf engine = nullptr;
    bool ok = create != nullptr && create(&engine, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_SUCCESS &&
              engine != nullptr && (*engine)->Realize(engine, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS;
    if (engine != nullptr) (*engine)->Destroy(engine);
    p.Check(ok, "slCreateEngine/Realize through dlopen(libOpenSLES.so)");
  }
  if (void* aa = dlopen("libaaudio.so", RTLD_NOW)) {
    auto create = reinterpret_cast<aaudio_result_t (*)(AAudioStreamBuilder**)>(dlsym(aa, "AAudio_createStreamBuilder"));
    auto del = reinterpret_cast<aaudio_result_t (*)(AAudioStreamBuilder*)>(dlsym(aa, "AAudioStreamBuilder_delete"));
    AAudioStreamBuilder* b = nullptr;
    p.Check(create != nullptr && del != nullptr && create(&b) == AAUDIO_OK && b != nullptr && del(b) == AAUDIO_OK,
            "AAudio_createStreamBuilder through dlopen(libaaudio.so)");
  }

  // libc symbols through RTLD_DEFAULT and through this library's own handle.
  p.Check(dlsym(RTLD_DEFAULT, "strlen") != nullptr, "dlsym(RTLD_DEFAULT, strlen)");
  Dl_info info{};
  p.Check(dladdr(reinterpret_cast<void*>(&Dlopen), &info) != 0 && info.dli_fname != nullptr, "dladdr");
  void* self = info.dli_fname != nullptr ? dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD) : nullptr;
  p.Check(self != nullptr && dlsym(self, "getpid") != nullptr, "dlsym(own handle, getpid)");

  // An app library by its bare name, from the directory this one came from;
  // libc++_shared.so, already loaded as a dependency, by name.
  void* helper = dlopen("libnative2_helper.so", RTLD_NOW);
  auto value = helper != nullptr ? reinterpret_cast<int (*)()>(dlsym(helper, "native2_helper_value")) : nullptr;
  p.Check(value != nullptr && value() == 42, std::string("dlopen(libnative2_helper.so) by bare name: ") +
                                                 (helper == nullptr ? (dlerror() ?: "") : "ok"));
  void* cxx = dlopen("libc++_shared.so", RTLD_NOW);
  p.Check(cxx != nullptr && dlsym(cxx, "_ZNSt6__ndk16chrono12steady_clock3nowEv") != nullptr,
          "dlopen(libc++_shared.so) finds the loaded one");
  dlerror();
  p.Check(dlopen("libnot-there.so", RTLD_NOW) == nullptr && dlerror() != nullptr, "dlopen of a missing library fails");
  p.Check(dlopen("/system/lib64/libnot-there.so", RTLD_NOW) == nullptr, "dlopen of a missing system library fails");
  return p;
}

// --- files ------------------------------------------------------------------------

Part Files(const std::string& dir) {
  Part p{"files"};
  errno = 0;
  std::string base = dir + "/native2-files";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);

  // flock: two opens of one file conflict; dup()s share a lock.
  std::string lockfile = base + "/lock";
  int a = open(lockfile.c_str(), O_CREAT | O_RDWR, 0644);
  int b = open(lockfile.c_str(), O_RDONLY);
  p.Check(a >= 0 && b >= 0, "open the lock file twice");
  p.Check(flock(a, LOCK_EX) == 0, "flock(LOCK_EX)");
  p.Check(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "a second LOCK_EX|LOCK_NB is EWOULDBLOCK");
  errno = 0;
  p.Check(flock(b, LOCK_SH | LOCK_NB) == -1 && errno == EWOULDBLOCK, "LOCK_SH|LOCK_NB while held is EWOULDBLOCK");
  errno = 0;
  p.Check(write(a, "data", 4) == 4, "write while locked");
  char rb[4];
  p.Check(pread(b, rb, 4, 0) == 4 && memcmp(rb, "data", 4) == 0, "the other open reads the locked file");
  p.Check(flock(a, LOCK_UN) == 0 && flock(b, LOCK_EX | LOCK_NB) == 0, "after LOCK_UN the other gets it");
  p.Check(flock(b, LOCK_SH) == 0 && flock(a, LOCK_SH | LOCK_NB) == 0, "two shared locks");
  p.Check(flock(a, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "LOCK_EX against a shared one fails");
  errno = 0;
  close(b);
  p.Check(flock(a, LOCK_EX | LOCK_NB) == 0, "closing the other open releases its lock");
  close(a);

  // The execute bit, where access(X_OK) holds.
  struct stat st;
  p.Check(stat(lockfile.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) != 0 && access(lockfile.c_str(), X_OK) == 0,
          "stat shows the execute bit access(X_OK) grants");
  p.notes.push_back([&] {
    char m[16];
    snprintf(m, sizeof(m), "mode %o", st.st_mode & 07777);
    return std::string(m);
  }());

  // Symbolic links.
  std::string target = base + "/target.txt", link = base + "/link.txt";
  { std::ofstream(target) << "linked"; }
  if (symlink(target.c_str(), link.c_str()) != 0) {
    p.notes.push_back(std::string("symlink: ") + strerror(errno) + " (no privilege to create links)");
    errno = 0;
  } else {
    p.Check(lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == static_cast<off_t>(target.size()),
            "lstat sees the link");
    p.Check(stat(link.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 6, "stat follows it");
    char lb[512] = {};
    ssize_t n = readlink(link.c_str(), lb, sizeof(lb) - 1);
    p.Check(n == static_cast<ssize_t>(target.size()) && target == lb, std::string("readlink: ") + lb);
    char r1[PATH_MAX], r2[PATH_MAX];
    p.Check(realpath(link.c_str(), r1) != nullptr && realpath(target.c_str(), r2) != nullptr && strcmp(r1, r2) == 0,
            "realpath resolves it");
    // A relative link, a link to a directory, std::filesystem.
    std::filesystem::create_directories(base + "/sub");
    { std::ofstream(base + "/sub/inner.txt") << "x"; }
    p.Check(symlink("sub", (base + "/dirlink").c_str()) == 0, "symlink to a directory (relative)");
    DIR* d = opendir((base + "/dirlink").c_str());
    bool found = false;
    if (d != nullptr) {
      while (dirent* e = readdir(d)) found |= strcmp(e->d_name, "inner.txt") == 0;
      closedir(d);
    }
    p.Check(found, "opendir through the directory link");
    p.Check(std::filesystem::is_symlink(base + "/dirlink") &&
                std::filesystem::read_symlink(base + "/dirlink") == std::filesystem::path("sub"),
            "std::filesystem::is_symlink/read_symlink");
    p.Check(symlink("target.txt", (base + "/rel.txt").c_str()) == 0, "symlink (relative file)");
    std::ifstream rel(base + "/rel.txt");
    std::string content;
    rel >> content;
    p.Check(content == "linked", "reading through a relative link");
    p.Check(unlink(link.c_str()) == 0 && access(target.c_str(), F_OK) == 0, "unlink removes the link, not the target");
    p.Check(unlink((base + "/dirlink").c_str()) == 0 && access((base + "/sub/inner.txt").c_str(), F_OK) == 0,
            "unlink of a directory link");
    p.Check(lstat(link.c_str(), &st) == -1 && errno == ENOENT, "the link is gone");
    errno = 0;
    p.notes.push_back("symlinks created");
  }

  // inotify: IN_MODIFY at the write, before the file is closed.
  std::string watched = base + "/watched";
  std::filesystem::create_directories(watched);
  std::string file = watched + "/w.txt";
  int fd = open(file.c_str(), O_CREAT | O_WRONLY, 0644);
  int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  p.Check(fd >= 0 && in >= 0, "open + inotify_init1");
  int wd = inotify_add_watch(in, watched.c_str(), IN_MODIFY | IN_CLOSE_WRITE);
  p.Check(wd > 0, "inotify_add_watch");
  auto wait_for = [&](uint32_t mask, int ms) {
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
      pollfd pf = {in, POLLIN, 0};
      poll(&pf, 1, 100);
      alignas(inotify_event) char ev[4096];
      ssize_t n = read(in, ev, sizeof(ev));
      for (ssize_t off = 0; n > 0 && off < n;) {
        auto* e = reinterpret_cast<inotify_event*>(ev + off);
        if ((e->mask & mask) != 0 && e->len > 0 && strcmp(e->name, "w.txt") == 0) return true;
        off += sizeof(inotify_event) + e->len;
      }
      timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if ((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000 > ms) return false;
    }
  };
  p.Check(write(fd, "hello", 5) == 5, "write");
  p.Check(wait_for(IN_MODIFY, 2000), "IN_MODIFY before close");
  close(fd);
  p.Check(wait_for(IN_CLOSE_WRITE, 3000), "IN_CLOSE_WRITE after close");
  close(in);
  std::filesystem::remove_all(base);
  return p;
}

// --- symbols ----------------------------------------------------------------------

std::vector<std::string> Symbols(const std::string& path) {
  std::vector<std::string> out;
  if (path.empty()) return out;
  std::ifstream f(path);
  if (!f) {
    out.push_back("native2 symbols: cannot read " + path);
    return out;
  }
  // set -> (strong missing, weak missing, checked)
  struct Tally {
    std::set<std::string> strong, weak;
    size_t checked = 0;
    size_t launcher = 0;
  };
  std::map<std::string, Tally> sets;
  std::string set, lib, sym, bind;
  while (f >> set >> lib >> sym >> bind) {
    Tally& t = sets[set];
    if (bind == "LAUNCHER" && sym == "__libc_init") {
      ++t.launcher;
      continue;
    }
    ++t.checked;
    if (dlsym(RTLD_DEFAULT, sym.c_str()) != nullptr) continue;
    (bind == "WEAK" ? t.weak : t.strong).insert(lib + ":" + sym);
  }
  for (auto& [name, t] : sets) {
    std::string line = "native2 symbols " + name + ": " + std::to_string(t.checked) + " checked, " +
                       std::to_string(t.strong.size()) + " missing";
    if (!t.weak.empty()) line += " (+" + std::to_string(t.weak.size()) + " weak)";
    if (!t.strong.empty()) {
      line += ":";
      for (const std::string& s : t.strong) line += " " + s;
    }
    if (!t.weak.empty()) {
      line += " | weak:";
      for (const std::string& s : t.weak) line += " " + s;
    }
    if (t.launcher != 0) line += " | executable startup: " + std::to_string(t.launcher) +
                                " __libc_init (gesher-exec program runs)";
    out.push_back(line);
  }
  return out;
}

#include "native3_unicode.inc"

#if defined(__aarch64__)
Part ArmFpcr() {
  Part p{"fpcr"};
  p.Check(Arm64FpcrContracts() == 0, "pinned FPCR rounding contracts");
  p.notes.push_back("pinned FPCR modes0..3, actual arithmetic, bits, pthread inheritance/isolation and nested callbacks");
  return p;
}
#endif

std::string Run(const std::string& dir, const std::string& symbols) {
  std::vector<Part> parts = {Extras(dir), Unicode(), Dlopen(), Files(dir)};
#if defined(__aarch64__)
  parts.push_back(ArmFpcr());
#endif
  std::string out;
  bool ok = true;
  for (const Part& p : parts) {
    out += p.Line() + "\n";
    ok &= p.failures.empty();
  }
  for (const std::string& s : Symbols(symbols)) out += s + "\n";
#if defined(__aarch64__)
  out += "native2: abi arm64-v8a\n";
#else
  out += "native2: abi x86_64\n";
#endif
  out += ok ? "native2: all passed\n" : "native2: FAILED\n";
  return out;
}

std::string Str(JNIEnv* env, jstring s) {
  if (s == nullptr) return "";
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c != nullptr ? c : "";
  if (c != nullptr) env->ReleaseStringUTFChars(s, c);
  return r;
}

jstring JNICALL NativeRun(JNIEnv* env, jclass, jstring dir, jstring symbols) {
  return env->NewStringUTF(Run(Str(env, dir), Str(env, symbols)).c_str());
}

bool PngWrite(void* context, const void* data, size_t size) {
  auto* bytes = static_cast<std::vector<uint8_t>*>(context);
  const auto* begin = static_cast<const uint8_t*>(data);
  bytes->insert(bytes->end(), begin, begin + size);
  return true;
}

bool PngAbort(void* context, const void*, size_t) {
  ++*static_cast<int*>(context);
  return false;
}

jstring JNICALL NativeBitmap(JNIEnv* env, jclass, jobject bitmap) {
  Part p{"bitmap"};
  void* library = dlopen("libjnigraphics.so", RTLD_NOW);
  p.Check(library != nullptr, "dlopen libjnigraphics");
  using Info = int (*)(JNIEnv*, jobject, AndroidBitmapInfo*);
  using Lock = int (*)(JNIEnv*, jobject, void**);
  using Unlock = int (*)(JNIEnv*, jobject);
  using Space = int32_t (*)(JNIEnv*, jobject);
  using Hardware = int (*)(JNIEnv*, jobject, void**);
  using Compress = int (*)(const AndroidBitmapInfo*, int32_t, const void*, int32_t, int32_t,
                           void*, bool (*)(void*, const void*, size_t));
  auto info = reinterpret_cast<Info>(dlsym(library, "AndroidBitmap_getInfo"));
  auto lock = reinterpret_cast<Lock>(dlsym(library, "AndroidBitmap_lockPixels"));
  auto unlock = reinterpret_cast<Unlock>(dlsym(library, "AndroidBitmap_unlockPixels"));
  auto space = reinterpret_cast<Space>(dlsym(library, "AndroidBitmap_getDataSpace"));
  auto hardware = reinterpret_cast<Hardware>(dlsym(library, "AndroidBitmap_getHardwareBuffer"));
  auto compress = reinterpret_cast<Compress>(dlsym(library, "AndroidBitmap_compress"));
  p.Check(info && lock && unlock && space && hardware && compress, "all six bitmap entry points");
  if (info && lock && unlock && space && hardware && compress) {
    AndroidBitmapInfo details{};
    p.Check(info(env, bitmap, &details) == 0 && details.width == 4 && details.height == 3 &&
                details.stride >= 16 && details.format == ANDROID_BITMAP_FORMAT_RGBA_8888,
            "width/height/stride/RGBA");
    p.Check(info(env, nullptr, &details) == ANDROID_BITMAP_RESULT_BAD_PARAMETER &&
                lock(nullptr, bitmap, nullptr) == ANDROID_BITMAP_RESULT_BAD_PARAMETER,
            "null bitmap/env rejected");
    int32_t data_space = space(env, bitmap);
    p.Check(data_space == 0x08810000, "sRGB data space");
    void* hardware_buffer = reinterpret_cast<void*>(1);
    p.Check(hardware(env, bitmap, &hardware_buffer) == ANDROID_BITMAP_RESULT_BAD_PARAMETER &&
                hardware_buffer == nullptr, "software bitmap has no hardware buffer");
    void* pixels = nullptr;
    bool locked = lock(env, bitmap, &pixels) == 0 && pixels != nullptr;
    p.Check(locked, "lock real pixels");
    if (locked) {
      auto* rgba = static_cast<uint8_t*>(pixels);
      p.Check(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
              "Java red pixel is RGBA");
      rgba[0] = 0;
      rgba[1] = 255;
      jclass bitmap_class = env->GetObjectClass(bitmap);
      jmethodID pixel = env->GetMethodID(bitmap_class, "getPixel", "(II)I");
      p.Check(env->CallIntMethod(bitmap, pixel, 0, 0) == static_cast<jint>(0xff00ff00),
              "native pixels alias Java Bitmap while locked");
      std::vector<uint8_t> png;
      p.Check(compress(&details, data_space, pixels, 1, 100, &png, PngWrite) == 0 &&
                  png.size() > 40 && memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0,
              "PNG compression calls native writer");
      int aborted = 0;
      p.Check(compress(&details, data_space, pixels, 1, 100, &aborted, PngAbort) != 0 && aborted > 0,
              "writer failure stops compression");
      p.Check(compress(&details, data_space, pixels, 1, 101, &png, PngWrite) ==
                  ANDROID_BITMAP_RESULT_BAD_PARAMETER, "invalid compression quality");
      p.Check(unlock(env, bitmap) == 0, "unlock pixels");
      if (!png.empty()) {
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(png.size()));
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(png.size()),
                                reinterpret_cast<const jbyte*>(png.data()));
        jclass factory = env->FindClass("android/graphics/BitmapFactory");
        jmethodID decode = env->GetStaticMethodID(factory, "decodeByteArray", "([BII)Landroid/graphics/Bitmap;");
        jobject decoded = env->CallStaticObjectMethod(factory, decode, bytes, 0, static_cast<jint>(png.size()));
        p.Check(decoded != nullptr && env->CallIntMethod(decoded, pixel, 0, 0) == static_cast<jint>(0xff00ff00),
                "PNG decoded by Java keeps native green pixel");
        if (decoded != nullptr) env->DeleteLocalRef(decoded);
        env->DeleteLocalRef(factory);
        env->DeleteLocalRef(bytes);
      }
      env->DeleteLocalRef(bitmap_class);
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    p.Check(false, "unexpected JNI exception");
  }
  if (library != nullptr) dlclose(library);
  p.notes.push_back("direct pixels and PNG callback round trip");
  return env->NewStringUTF(p.Line().c_str());
}

#include "native3_fonts.inc"

}  // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  static const JNINativeMethod methods[] = {
      {"run", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(&NativeRun)}};
  bool registered = false;
  for (const char* cls : {"gesher/Native2Test", "com/gesher/native2/Native2Activity"}) {
    jclass c = env->FindClass(cls);
    if (c == nullptr) {
      env->ExceptionClear();
      continue;
    }
    registered |= env->RegisterNatives(c, methods, 1) == JNI_OK;
    env->DeleteLocalRef(c);
  }
  // The original dalvikvm checks intentionally use only the core boot jars.
  // Register bitmap methods only when this process has the framework too.
  jclass bitmap_class = env->FindClass("android/graphics/Bitmap");
  if (bitmap_class == nullptr) {
    env->ExceptionClear();
    return registered ? JNI_VERSION_1_6 : JNI_ERR;
  }
  env->DeleteLocalRef(bitmap_class);
  static const JNINativeMethod bitmap_method = {
      "bitmap", "(Landroid/graphics/Bitmap;)Ljava/lang/String;", reinterpret_cast<void*>(&NativeBitmap)};
  for (const char* cls : {"gesher/Native3BitmapTest", "com/gesher/native2/Native2Activity"}) {
    jclass c = env->FindClass(cls);
    if (c == nullptr) { env->ExceptionClear(); continue; }
    registered |= env->RegisterNatives(c, &bitmap_method, 1) == JNI_OK;
    env->DeleteLocalRef(c);
  }
  static const JNINativeMethod font_method = {
      "fonts", "()Ljava/lang/String;", reinterpret_cast<void*>(&NativeFonts)};
  for (const char* cls : {"gesher/Native3FontTest", "com/gesher/native2/Native2Activity"}) {
    jclass c = env->FindClass(cls);
    if (c == nullptr) { env->ExceptionClear(); continue; }
    registered |= env->RegisterNatives(c, &font_method, 1) == JNI_OK;
    env->DeleteLocalRef(c);
  }
  return registered ? JNI_VERSION_1_6 : JNI_ERR;
}
