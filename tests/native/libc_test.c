// An x86-64 Android shared object that uses bionic's libc, libm, libdl,
// liblog and pthread, for the bionic shim's check (compat/bionic). Built by
// the NDK in .github/workflows/native-libs.yml *with* libc (DT_NEEDED libc.so,
// libm.so, libdl.so, liblog.so) but with -fno-stack-protector and no
// thread_local, so it does not touch %fs (docs/decisions/0014).
//
// elf_load_test calls gesher_selftest(), which runs every check, prints each
// result with printf (itself a check of the shim's variadic bridge) and
// returns the number of failures.

#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <math.h>
#include <mntent.h>
#include <pthread.h>
#include <search.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-48s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static void check_str(const char* what, const char* got, const char* want) {
  int ok = strcmp(got, want) == 0;
  if (ok) {
    check(what, 1);
  } else {
    printf("  %-48s FAIL  got \"%s\", want \"%s\"\n", what, got, want);
    ++failures;
  }
}

#if defined(__x86_64__)
#include "timerfd_syscall_test.inc"
#endif
#include "tgnet_event_loop_test.inc"
#include "epoll_nested_test.inc"
#include "allocation_test.inc"
#include "system_contract_test.inc"
#include "socket_control_test.inc"
#include "directory_contract_test.inc"
#include "semaphore_contract_test.inc"
#include "file_allocation_contract_test.inc"
#include "positioned_io_contract_test.inc"
#include "memfd_contract_test.inc"

// --- memory and strings -------------------------------------------------------

static void test_memory(void) {
  char* p = malloc(100);
  check("malloc", p != NULL && ((uintptr_t)p & 15) == 0);
  memset(p, 'x', 99);
  p[99] = 0;
  p = realloc(p, 1000);
  check("realloc keeps contents", p != NULL && strlen(p) == 99);
  free(p);
  int* z = calloc(256, sizeof(int));
  int all_zero = 1;
  for (int i = 0; i < 256; ++i) all_zero &= z[i] == 0;
  check("calloc zeroes", all_zero);
  free(z);
  void* a = NULL;
  check("posix_memalign(256)", posix_memalign(&a, 256, 1000) == 0 && ((uintptr_t)a & 255) == 0);
  memset(a, 1, 1000);
  a = realloc(a, 2000);
  check("realloc of aligned memory", a != NULL && ((unsigned char*)a)[999] == 1);
  free(a);
}

static void test_strings(void) {
  char* d = strdup("gesher");
  check_str("strdup", d, "gesher");
  free(d);
  check("strlen", strlen("bionic") == 6);
  check("strcmp", strcmp("a", "b") < 0 && strcmp("b", "a") > 0);
  check("strchr", strchr("hello", 'l') != NULL && *strchr("hello", 'l') == 'l');
  // long is 64-bit under bionic LP64.
  long big = strtol("9000000000", NULL, 10);
  check("strtol returns a 64-bit long", big == 9000000000L);
  check("strtoul hex", strtoul("ffffffffff", NULL, 16) == 0xffffffffffUL);
  check("atoi", atoi("-42") == -42);
}

// bionic's ctype table, read directly by code built for an old API level
// (WhatsApp's libar-bundle3.so: `_ctype_[1 + c] & 7` is isalnum).
extern const char* _ctype_;

static void test_ctype_table(void) {
  int ok = _ctype_ != NULL && _ctype_[0] == 0;
  ok = ok && (_ctype_[1 + 'Q'] & 0x01) && (_ctype_[1 + 'q'] & 0x02) && (_ctype_[1 + '7'] & 0x04);
  ok = ok && (_ctype_[1 + 'f'] & 0x40) && !(_ctype_[1 + 'g'] & 0x40);
  ok = ok && (_ctype_[1 + ' '] & 0x08) && (_ctype_[1 + 9] & 0x20) && (_ctype_[1 + '_'] & 0x10);
  ok = ok && (_ctype_[1 + '_'] & 7) == 0 && _ctype_[1 + 200] == 0;
  check("_ctype_ classes ASCII as the C locale does", ok);
}

// /dev/urandom through stdio: mbedtls seeds its DRBG with fread from
// fopen("/dev/urandom"), and WhatsApp aborts when that yields nothing. The
// device is a NUL handle that read() fills; a FILE's own reads must use it too.
static void test_urandom_stdio(void) {
  unsigned char a[64], b[64];
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  FILE* f = fopen("/dev/urandom", "rb");
  check("fopen(\"/dev/urandom\")", f != NULL);
  if (f == NULL) return;
  setbuf(f, NULL);  // as mbedtls does
  size_t n = fread(a, 1, sizeof(a), f);
  size_t m = fread(b, sizeof(b), 1, f);
  int c = fgetc(f);
  fclose(f);
  int nonzero = 0;
  for (size_t i = 0; i < sizeof(a); ++i) nonzero |= a[i];
  check("fread of /dev/urandom fills the buffer", n == sizeof(a) && m == 1 && nonzero);
  check("two reads of /dev/urandom differ", memcmp(a, b, sizeof(a)) != 0);
  check("fgetc of /dev/urandom gives a byte", c != EOF);
  FILE* z = fopen("/dev/zero", "rb");
  memset(a, 0xAA, sizeof(a));
  check("fread of /dev/zero gives zeros",
        z != NULL && fread(a, 1, sizeof(a), z) == sizeof(a) && a[0] == 0 && a[63] == 0);
  if (z != NULL) fclose(z);
}

static void test_format(void) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%d %ld %s %c %x %%", -7, 9000000000L, "str", 'Q', 255);
  check_str("snprintf ints, long, string, char", buf, "-7 9000000000 str Q ff %");
  snprintf(buf, sizeof(buf), "[%5d|%-5d|%05d]", 42, 42, 42);
  check_str("snprintf widths", buf, "[   42|42   |00042]");
  snprintf(buf, sizeof(buf), "%.2f %g %e", 3.14159, 0.5, 1000.0);
  check_str("snprintf doubles", buf, "3.14 0.5 1.000000e+03");
  snprintf(buf, sizeof(buf), "%zu %lld %llu %hhd", (size_t)123, -5LL, 18446744073709551615ULL,
           (signed char)-1);
  check_str("snprintf size_t, long long", buf, "123 -5 18446744073709551615 -1");
  snprintf(buf, sizeof(buf), "%*d|%.*s", 6, 1, 3, "abcdef");
  check_str("snprintf * width and precision", buf, "     1|abc");
  snprintf(buf, sizeof(buf), "%p", (void*)0x1234);
  check_str("snprintf %p", buf, "0x1234");
  // More arguments than registers: the rest come from the stack.
  snprintf(buf, sizeof(buf), "%d %d %d %d %d %d %d %d %f %f %f %f %f %f %f %f %f %f", 1, 2, 3,
           4, 5, 6, 7, 8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0);
  check_str("snprintf stack arguments", buf,
            "1 2 3 4 5 6 7 8 1.000000 2.000000 3.000000 4.000000 5.000000 6.000000 7.000000 "
            "8.000000 9.000000 10.000000");
  int n = snprintf(buf, 4, "abcdef");
  check("snprintf truncates and counts", n == 6 && strcmp(buf, "abc") == 0);

  int i = 0;
  double f = 0;
  char word[16] = {0};
  long l = 0;
  int got = sscanf("12 34.5 abc 7000000000", "%d %lf %15s %ld", &i, &f, word, &l);
  check("sscanf", got == 4 && i == 12 && f == 34.5 && strcmp(word, "abc") == 0 &&
                      l == 7000000000L);
}

// --- errno, files, time ---------------------------------------------------------

static void test_errno_files(void) {
  errno = 0;
  int fd = open("/gesher-no-such-dir/no-such-file", O_RDONLY);
  check("open fails with ENOENT", fd == -1 && errno == ENOENT);
  FILE* out = stdout;
  check("stdout is usable", out != NULL && fwrite("  (fwrite to stdout)\n", 1, 21, out) == 21);
}

static void test_time_math(void) {
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  struct timespec req = {0, 20 * 1000 * 1000};
  nanosleep(&req, NULL);
  clock_gettime(CLOCK_MONOTONIC, &b);
  long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
  check("nanosleep and CLOCK_MONOTONIC", ms >= 15 && b.tv_nsec >= 0 && b.tv_nsec < 1000000000);
  struct timespec rt;
  clock_gettime(CLOCK_REALTIME, &rt);
  check("CLOCK_REALTIME is after 2020", rt.tv_sec > 1577836800);
  check("libm", fabs(sqrt(2.0) * sqrt(2.0) - 2.0) < 1e-12 && fabs(sin(0.0)) < 1e-12 &&
                    fabs(pow(2.0, 10.0) - 1024.0) < 1e-9);
}

// statfs and its neighbours: imports the shim once lacked, so they were bound
// to stubs that abort when called (Material Files died in statfs from
// libselinux-jni.so). statfs on the working directory and on Android's /data
// (not on the disk: it answers for the volume it would be on, as
// SQLiteGlobal's StatFs("/data") needs), fstatfs on an open file, the xattr
// family failing with ENOTSUP, an empty mount table and mount refused.
// Acquire loads of read-only memory (LocalSend's libflutter: Dart reads its
// write-protected VM isolate heap with LDAR). The arm64 translator's dynarmic
// emits an ordered load as `lock xadd [m], 0`, which faults as a write on a
// read-only page; the translator completes the load instead.
static void test_ldar_readonly(void) {
#if defined(__aarch64__)
  long page = sysconf(_SC_PAGESIZE);
  uint64_t* data = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check("mmap for guest acquire loads", data != MAP_FAILED);
  if (data == MAP_FAILED) return;
  const uint64_t v = 0x8899AABBCCDDEEF1ull;
  data[8] = v;
  check("mprotect PROT_READ", mprotect(data, page, PROT_READ) == 0);
  uint64_t x;
  uint32_t w, h, b;
  __asm__ volatile("ldar %0, [%1]" : "=r"(x) : "r"(&data[8]) : "memory");
  __asm__ volatile("ldar %w0, [%1]" : "=r"(w) : "r"(&data[8]) : "memory");
  __asm__ volatile("ldarh %w0, [%1]" : "=r"(h) : "r"(&data[8]) : "memory");
  __asm__ volatile("ldarb %w0, [%1]" : "=r"(b) : "r"(&data[8]) : "memory");
  check("LDAR/LDARH/LDARB of a read-only page",
        x == v && w == (uint32_t)v && h == (uint16_t)v && b == (uint8_t)v);
  check("read-only page unchanged after acquire loads", data[8] == v);
  munmap(data, page);
#endif
}

static void test_statfs(void) {
  char cwd[1024];
  struct statfs st;
  memset(&st, 0, sizeof(st));
  int ok = getcwd(cwd, sizeof(cwd)) != NULL && statfs(cwd, &st) == 0;
  check("statfs(cwd)", ok);
  check("statfs: f_bsize > 0, f_blocks > 0", ok && st.f_bsize > 0 && st.f_blocks > 0);
  check("statfs: free <= total, an ext4 f_type",
        ok && st.f_bfree <= st.f_blocks && st.f_bavail <= st.f_blocks && st.f_type == 0xEF53);
  struct statfs data;
  memset(&data, 0, sizeof(data));
  check("statfs(\"/data\"): f_bsize > 0, f_blocks > 0",
        statfs("/data", &data) == 0 && data.f_bsize > 0 && data.f_blocks > 0);
  struct statfs64 st64;
  memset(&st64, 0, sizeof(st64));
  check("statfs64(cwd)", ok && statfs64(cwd, &st64) == 0 && st64.f_bsize == st.f_bsize &&
                             st64.f_blocks == st.f_blocks);

  char tmp[1100];
  snprintf(tmp, sizeof(tmp), "%s/libc_test_statfs.tmp", ok ? cwd : ".");
  int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0600);
  struct statfs fst;
  memset(&fst, 0, sizeof(fst));
  check("fstatfs(an open file)",
        fd >= 0 && fstatfs(fd, &fst) == 0 && fst.f_bsize > 0 && fst.f_blocks > 0);
  char value[16];
  errno = 0;
  check("fgetxattr fails with ENOTSUP",
        fd >= 0 && fgetxattr(fd, "user.x", value, sizeof(value)) == -1 && errno == ENOTSUP);
  errno = 0;
  check("fsetxattr fails with ENOTSUP",
        fd >= 0 && fsetxattr(fd, "user.x", "1", 1, 0) == -1 && errno == ENOTSUP);
  if (fd >= 0) close(fd);
  errno = 0;
  check("getxattr fails with ENOTSUP",
        getxattr(tmp, "security.selinux", value, sizeof(value)) == -1 && errno == ENOTSUP);
  errno = 0;
  check("lgetxattr fails with ENOTSUP",
        lgetxattr(tmp, "security.selinux", value, sizeof(value)) == -1 && errno == ENOTSUP);
  errno = 0;
  check("setxattr fails with ENOTSUP",
        setxattr(tmp, "user.inode_cache", "1", 1, 0) == -1 && errno == ENOTSUP);
  errno = 0;
  check("lsetxattr fails with ENOTSUP", lsetxattr(tmp, "user.x", "1", 1, 0) == -1 &&
                                            errno == ENOTSUP);
  char list[64];
  errno = 0;
  check("llistxattr fails with ENOTSUP",
        llistxattr(tmp, list, sizeof(list)) == -1 && errno == ENOTSUP);
  errno = 0;
  check("removexattr fails with ENOTSUP", removexattr(tmp, "user.x") == -1 && errno == ENOTSUP);
  unlink(tmp);

  FILE* mounts = setmntent("/proc/mounts", "re");
  check("setmntent(/proc/mounts) opens a stream", mounts != NULL);
  check("getmntent: no entries", mounts == NULL || getmntent(mounts) == NULL);
  if (mounts != NULL) check("endmntent", endmntent(mounts) == 1);
  errno = 0;
  check("mount fails with EPERM",
        mount("none", "/gesher-no-such-dir", "tmpfs", 0, NULL) == -1 && errno == EPERM);
}

// Android's system CA directory under its Android names (compat/bionic/
// system_cacerts.h, docs/network.md): what Dart's dart:io and BoringSSL's
// by-dir lookup do with it. Checked when GESHER_TEST_SYSTEM_CACERTS=1 (the
// x86-64 track sets it when cacerts\ is next to elf_load_test.exe); otherwise
// a missing directory is reported, not failed.
static int is_cert_name(const char* name) {
  for (int i = 0; i < 8; ++i) {
    char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
  }
  return strcmp(name + 8, ".0") == 0;
}

static int file_is_pem(const char* path) {
  FILE* f = fopen(path, "r");
  if (f == NULL) return 0;
  static char buf[65536];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  return n > 0 && strstr(buf, "-----BEGIN CERTIFICATE-----") != NULL;
}

static void test_system_cacerts(void) {
  static const char kDir[] = "/system/etc/security/cacerts";
  static const char kApex[] = "/apex/com.android.conscrypt/cacerts";
  const char* strict_env = getenv("GESHER_TEST_SYSTEM_CACERTS");
  int strict = strict_env != NULL && strcmp(strict_env, "1") == 0;
  struct stat st;
  errno = 0;
  int is_dir = stat(kDir, &st) == 0 && S_ISDIR(st.st_mode);
  if (!is_dir && !strict) {
    printf("  INFO  %s: not found (errno %d); GESHER_TEST_SYSTEM_CACERTS unset, not checked\n",
           kDir, errno);
    return;
  }
  check("stat(/system/etc/security/cacerts): a directory", is_dir);
  struct stat64 st64;
  check("fstatat64(AT_FDCWD, cacerts, 0): a directory (Dart)",
        fstatat64(AT_FDCWD, kDir, &st64, 0) == 0 && S_ISDIR(st64.st_mode));
  check("lstat(cacerts/): a directory", lstat("/system/etc/security/cacerts/", &st) == 0 &&
                                            S_ISDIR(st.st_mode));

  char name[256] = "";
  DIR* d = opendir(kDir);
  check("opendir(/system/etc/security/cacerts)", d != NULL);
  if (d != NULL) {
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
      if (strlen(e->d_name) == 10 && is_cert_name(e->d_name)) {
        snprintf(name, sizeof(name), "%s", e->d_name);
        break;
      }
    }
    closedir(d);
  }
  check("cacerts has a <subject hash>.0 entry", name[0] != '\0');
  if (name[0] == '\0') return;

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", kDir, name);
  check("stat(cacerts/<hash>.0): a regular file", stat(path, &st) == 0 && S_ISREG(st.st_mode));
  check("fopen+fread(cacerts/<hash>.0): PEM (BoringSSL by-dir)", file_is_pem(path));
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  check("open(cacerts/<hash>.0)", fd >= 0);
  if (fd >= 0) close(fd);

  // The Conscrypt APEX's name (newer Dart probes it first) is the same directory.
  d = opendir(kApex);
  int apex_entry = 0;
  if (d != NULL) {
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
        apex_entry = 1;
        break;
      }
    }
    closedir(d);
  }
  check("opendir(/apex/com.android.conscrypt/cacerts): has entries", apex_entry);
  snprintf(path, sizeof(path), "%s/%s", kApex, name);
  check("fopen(/apex/.../cacerts/<hash>.0): PEM", file_is_pem(path));

  // Nothing escapes the directory, and only that exact name maps.
  errno = 0;
  check("stat(cacerts/../cacerts) fails with ENOENT",
        stat("/system/etc/security/cacerts/../cacerts", &st) == -1 && errno == ENOENT);
  errno = 0;
  check("stat(cacerts/..\cacerts) fails with ENOENT",
        stat("/system/etc/security/cacerts/..\cacerts", &st) == -1 && errno == ENOENT);
  snprintf(path, sizeof(path), "%s/%s:x", kDir, name);
  errno = 0;
  check("fopen(cacerts/<hash>.0:x) fails with ENOENT", fopen(path, "r") == NULL && errno == ENOENT);
  errno = 0;
  check("open(cacerts/NUL) fails with ENOENT",
        open("/system/etc/security/cacerts/NUL", O_RDONLY) == -1 && errno == ENOENT);
  check("stat(/system/etc/security/cacertsX) is not mapped",
        stat("/system/etc/security/cacertsX", &st) == -1);
}

static int compare_ints(const void* a, const void* b) {
  return *(const int*)a - *(const int*)b;
}

static void test_qsort(void) {
  int v[] = {5, 3, 9, 1, 7, 2, 8};
  qsort(v, 7, sizeof(int), compare_ints);
  int sorted = 1;
  for (int i = 1; i < 7; ++i) sorted &= v[i - 1] <= v[i];
  check("qsort with a .so comparator", sorted);
  int key = 7;
  check("bsearch", bsearch(&key, v, 7, sizeof(int), compare_ints) != NULL);
}

#include "search_test.inc"
#include "pthread_cleanup_test.inc"

// --- pthread ----------------------------------------------------------------------

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_counter = 0;

static void* add_many(void* arg) {
  for (int i = 0; i < 10000; ++i) {
    pthread_mutex_lock(&g_mutex);
    ++g_counter;
    pthread_mutex_unlock(&g_mutex);
  }
  return arg;
}

static pthread_mutex_t g_cv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_stage = 0;

static void* ping(void* arg) {
  (void)arg;
  pthread_mutex_lock(&g_cv_mutex);
  while (g_stage != 1) pthread_cond_wait(&g_cv, &g_cv_mutex);
  g_stage = 2;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_cv_mutex);
  return NULL;
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_runs = 0;
static void once_init(void) { ++g_once_runs; }

static pthread_key_t g_key;
static int g_key_dtor_runs = 0;
static void key_dtor(void* value) {
  if (value == (void*)0x42) __atomic_fetch_add(&g_key_dtor_runs, 1, __ATOMIC_SEQ_CST);
}

static void* key_thread(void* arg) {
  pthread_once(&g_once, once_init);
  pthread_setspecific(g_key, (void*)0x42);
  return pthread_getspecific(g_key) == (void*)0x42 ? arg : NULL;
}

static pthread_mutex_t g_rec;
static void* try_rec(void* arg) {
  (void)arg;
  return (void*)(intptr_t)pthread_mutex_trylock(&g_rec);
}

static void test_pthread(void) {
  pthread_t t[4];
  for (int i = 0; i < 4; ++i) pthread_create(&t[i], NULL, add_many, (void*)(intptr_t)(i + 1));
  int rets_ok = 1;
  for (int i = 0; i < 4; ++i) {
    void* ret = NULL;
    pthread_join(t[i], &ret);
    rets_ok &= ret == (void*)(intptr_t)(i + 1);
  }
  check("4 threads, mutex-protected counter", g_counter == 40000);
  check("pthread_join returns the thread's value", rets_ok);

  pthread_t p;
  pthread_create(&p, NULL, ping, NULL);
  pthread_mutex_lock(&g_cv_mutex);
  g_stage = 1;
  pthread_cond_broadcast(&g_cv);
  while (g_stage != 2) pthread_cond_wait(&g_cv, &g_cv_mutex);
  pthread_mutex_unlock(&g_cv_mutex);
  pthread_join(p, NULL);
  check("condition variable handshake", g_stage == 2);

  pthread_key_create(&g_key, key_dtor);
  pthread_t k[3];
  int key_ok = 1;
  for (int i = 0; i < 3; ++i) pthread_create(&k[i], NULL, key_thread, (void*)1);
  for (int i = 0; i < 3; ++i) {
    void* ret = NULL;
    pthread_join(k[i], &ret);
    key_ok &= ret == (void*)1;
  }
  check("pthread_once runs once", g_once_runs == 1);
  check("thread-specific values", key_ok && pthread_getspecific(g_key) == NULL);
  check("key destructors run at thread exit", g_key_dtor_runs == 3);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&g_rec, &attr);
  int rec_ok = pthread_mutex_lock(&g_rec) == 0 && pthread_mutex_lock(&g_rec) == 0;
  pthread_t r;
  void* other = NULL;
  pthread_create(&r, NULL, try_rec, NULL);
  pthread_join(r, &other);
  rec_ok &= other == (void*)(intptr_t)EBUSY;
  rec_ok &= pthread_mutex_unlock(&g_rec) == 0 && pthread_mutex_unlock(&g_rec) == 0;
  rec_ok &= pthread_mutex_trylock(&g_rec) == 0 && pthread_mutex_unlock(&g_rec) == 0;
  check("recursive mutex", rec_ok);

  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_nsec += 30 * 1000 * 1000;
  if (deadline.tv_nsec >= 1000000000) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&g_cv_mutex);
  int tw = pthread_cond_timedwait(&g_cv, &g_cv_mutex, &deadline);
  pthread_mutex_unlock(&g_cv_mutex);
  check("pthread_cond_timedwait times out", tw == ETIMEDOUT);
  check("pthread_self is stable", pthread_equal(pthread_self(), pthread_self()));
}

// --- libdl and liblog -------------------------------------------------------------

static int phdr_found = 0;
static int phdr_callback(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD) continue;
    uintptr_t start = info->dlpi_addr + ph->p_vaddr;
    if ((uintptr_t)data >= start && (uintptr_t)data < start + ph->p_memsz) {
      phdr_found = 1;
      return 1;
    }
  }
  return 0;
}

int gesher_selftest(void);

static void test_random_state(void) {
  typedef char* (*init_fn)(unsigned, char*, size_t);
  typedef char* (*set_fn)(char*);
  init_fn initialize = (init_fn)dlsym(RTLD_DEFAULT, "initstate");
  set_fn select_state = (set_fn)dlsym(RTLD_DEFAULT, "setstate");
  check("initstate/setstate are real exports", initialize != NULL && select_state != NULL);
  if (initialize == NULL || select_state == NULL) return;
  uint32_t first[32], second[64];
  char* original = initialize(1, (char*)first, sizeof(first));
  check("BSD random known seed-1 sequence", original != NULL
      && random() == 1804289383L && random() == 846930886L);
  char* previous = initialize(99, (char*)second, sizeof(second));
  check("initstate returns previous caller state", previous == (char*)first);
  random(); random();
  check("setstate resumes independent sequence", select_state((char*)first) == (char*)second
      && random() == 1681692777L);
  check("setstate of current buffer preserves cursor", select_state((char*)first) == (char*)first
      && random() == 1714636915L);
  srandom(1);
  check("srandom reseeds selected state", random() == 1804289383L);
  errno = 0;
  check("initstate rejects short buffer", initialize(5, (char*)second, 7) == NULL && errno == EINVAL
      && random() == 846930886L);
  uint32_t invalid = 0x7fffffff;
  errno = 0;
  check("setstate rejects invalid cursor without mutation", select_state((char*)&invalid) == NULL
      && errno == EINVAL && random() == 1681692777L);
  const size_t sizes[] = {8, 32, 64, 128, 256};
  for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i) {
    uint32_t guarded[66];
    memset(guarded, 0xa5, sizeof(guarded));
    initialize(7, (char*)&guarded[1], sizes[i]);
    long sample = random();
    srandom(7);
    check("random state size reseeds deterministically", random() == sample);
    check("random state keeps buffer canaries", guarded[0] == 0xa5a5a5a5u
        && guarded[1 + sizes[i]/sizeof(uint32_t)] == 0xa5a5a5a5u);
    // Do not retain the loop's caller-owned stack buffer after its scope ends.
    select_state(original);
  }
  select_state(original);
}

static void test_dl(void) {
  void* self = dlsym(RTLD_DEFAULT, "gesher_selftest");
  check("dlsym(RTLD_DEFAULT) finds the library's export", self == (void*)gesher_selftest);
  check("dlsym of a missing symbol fails", dlsym(RTLD_DEFAULT, "gesher_no_such") == NULL &&
                                               dlerror() != NULL);
  dl_iterate_phdr(phdr_callback, (void*)gesher_selftest);
  check("dl_iterate_phdr reports this library", phdr_found);
  Dl_info info;
  check("dladdr names the library", dladdr((void*)gesher_selftest, &info) != 0 &&
                                        strstr(info.dli_fname, "libc_test") != NULL);
  __android_log_print(ANDROID_LOG_INFO, "gesher", "liblog: %s %d", "formatted", 42);
  check("__android_log_print", 1);
  test_random_state();

  // Apps such as VLC forward a redirected stderr pipe to liblog. Liblog must
  // retain its original output destination rather than echoing into that pipe.
  int capture[2] = {-1, -1};
  int saved = dup(STDERR_FILENO);
  int ready = saved >= 0 && pipe(capture) == 0;
  int redirected = ready && dup2(capture[1], STDERR_FILENO) == STDERR_FILENO;
  int logged = redirected ? __android_log_write(ANDROID_LOG_ERROR, "gesher",
      "liblog keeps its original sink after app dup2") : -1;
  int restored = saved >= 0 && dup2(saved, STDERR_FILENO) == STDERR_FILENO;
  if (saved >= 0) close(saved);
  if (capture[1] >= 0) close(capture[1]);
  char echoed[256];
  ssize_t count = -1;
  if (capture[0] >= 0) {
    // Bound this diagnostic even if restoration fails.
    int flags = fcntl(capture[0], F_GETFL);
    if (flags >= 0 && fcntl(capture[0], F_SETFL, flags | O_NONBLOCK) == 0) {
      count = read(capture[0], echoed, sizeof(echoed));
    }
    close(capture[0]);
  }
  check("liblog bypasses app stderr redirection", redirected && logged > 0 && restored && count == 0);
}

__attribute__((visibility("default"))) int gesher_selftest(void) {
  printf("libc_test: bionic libc through the shim\n");
  test_memory();
  test_allocation_contracts();
  test_system_contracts();
  test_socket_control_contracts();
  test_directory_contracts();
  test_semaphore_contracts();
  test_file_allocation_contracts();
  test_positioned_io_contracts();
  test_memfd_contracts();
#if defined(__x86_64__)
  test_timerfd_syscalls();
#endif
  test_strings();
  test_ctype_table();
  test_urandom_stdio();
  test_format();
  test_errno_files();
  test_statfs();
  test_system_cacerts();
  test_ldar_readonly();
  test_time_math();
  test_qsort();
  test_search();
  test_pthread();
  test_pthread_cleanup();
  test_tgnet_event_loop();
  test_epoll_nested();
  test_dl();
  if (failures == 0) {
    printf("libc_test: all passed\n");
  } else {
    printf("libc_test: %d check(s) failed\n", failures);
  }
  fflush(stdout);
  return failures;
}
