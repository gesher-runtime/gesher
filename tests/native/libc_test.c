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
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
}

__attribute__((visibility("default"))) int gesher_selftest(void) {
  printf("libc_test: bionic libc through the shim\n");
  test_memory();
  test_strings();
  test_format();
  test_errno_files();
  test_time_math();
  test_qsort();
  test_pthread();
  test_dl();
  if (failures == 0) {
    printf("libc_test: all passed\n");
  } else {
    printf("libc_test: %d check(s) failed\n", failures);
  }
  fflush(stdout);
  return failures;
}
