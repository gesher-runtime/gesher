// An arm64-v8a Android .so (NDK, API 29, -fstack-protector-all) that checks
// the parts of the arm64 bridge where the guest ABI differs from the host's
// (docs/arm64.md, ADR 0047). Loaded by arm64_elf_test, which calls
// gesher_selftest() through a host_entry stub and gesher_perf_kernel() for the
// overhead measurement.
//
//  - ELF TLS (__thread through TLSDESC) in the main and in new threads, and the
//    stack protector (its guard is a TLS slot, [TPIDR_EL0 + 40]);
//  - setjmp/longjmp (guest code in the translator);
//  - struct stat and struct epoll_event (layouts differ from x86-64);
//  - syscall(2) with arm64 numbers; getauxval(AT_HWCAP);
//  - C11 atomics from four threads (LL/SC through the exclusive monitor);
//  - va_list: the guest's own variadic wrappers around vsnprintf (AAPCS64
//    va_list converted to System V), with more than six integer arguments;
//  - a host function pointer from dlsym called directly by guest code.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "arm64_perf_kernel.h"

static int failures;

static void check(const char* what, int ok) {
  printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok) ++failures;
}

// --- TLS ------------------------------------------------------------------------
static __thread int tl_value = 5;
static __thread char tl_buf[64] = "initial";

static void* tls_thread(void* arg) {
  int ok = tl_value == 5 && strcmp(tl_buf, "initial") == 0;
  tl_value = (int)(intptr_t)arg;
  strcpy(tl_buf, "thread");
  ok = ok && tl_value == (int)(intptr_t)arg;
  return (void*)(intptr_t)ok;
}

static void test_tls(void) {
  check("__thread initial value", tl_value == 5 && strcmp(tl_buf, "initial") == 0);
  tl_value = 99;
  pthread_t t[3];
  for (int i = 0; i < 3; ++i) pthread_create(&t[i], NULL, tls_thread, (void*)(intptr_t)(i + 10));
  int ok = 1;
  for (int i = 0; i < 3; ++i) {
    void* r = NULL;
    pthread_join(t[i], &r);
    ok = ok && r == (void*)1;
  }
  check("__thread is per thread (3 threads)", ok);
  check("__thread survives other threads", tl_value == 99 && strcmp(tl_buf, "initial") == 0);
}

// --- setjmp / longjmp -------------------------------------------------------------
static jmp_buf g_jmp;
static int g_depth;

static void __attribute__((noinline)) jump_from_depth(int n) {
  char pad[100];
  snprintf(pad, sizeof(pad), "%d", n);
  g_depth = n;
  if (n == 0) longjmp(g_jmp, 7);
  jump_from_depth(n - 1);
}

static void test_setjmp(void) {
  volatile int local = 1;
  int r = setjmp(g_jmp);
  if (r == 0) {
    local = 2;
    jump_from_depth(5);
    check("longjmp returned to setjmp", 0);
    return;
  }
  check("setjmp returns longjmp's value (7)", r == 7);
  check("locals survive longjmp", local == 2 && g_depth == 0);
  sigjmp_buf sj;
  int s = sigsetjmp(sj, 1);
  if (s == 0) siglongjmp(sj, 0);
  check("siglongjmp(env, 0) makes sigsetjmp return 1", s == 1);
}

// --- stat, epoll --------------------------------------------------------------------
static void test_stat_epoll(const char* dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/arm64_abi_test.tmp", dir);
  FILE* f = fopen(path, "wb");
  char data[1234];
  memset(data, 'x', sizeof(data));
  if (f) {
    fwrite(data, 1, sizeof(data), f);
    fclose(f);
  }
  struct stat st;
  memset(&st, 0xAB, sizeof(st));
  int r = stat(path, &st);
  check("stat: st_size, S_ISREG (arm64 struct stat)",
        r == 0 && st.st_size == 1234 && S_ISREG(st.st_mode) && st.st_nlink >= 1);
  int fd = open(path, O_RDONLY);
  struct stat fst;
  memset(&fst, 0, sizeof(fst));
  check("fstat agrees with stat",
        fd >= 0 && fstat(fd, &fst) == 0 && fst.st_size == 1234 && fst.st_mode == st.st_mode);
  struct stat lst;
  check("lstat", lstat(path, &lst) == 0 && lst.st_size == 1234);
  struct stat ast;
  check("fstatat", fstatat(AT_FDCWD, path, &ast, 0) == 0 && ast.st_size == 1234);
  if (fd >= 0) close(fd);
  unlink(path);

  int p[2];
  int ep = epoll_create1(0);
  int ok = ep >= 0 && pipe(p) == 0;
  if (ok) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    ok = epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0;
    ok = ok && write(p[1], "!", 1) == 1;
    struct epoll_event out[4];
    memset(out, 0, sizeof(out));
    int n = ok ? epoll_wait(ep, out, 4, 1000) : -1;
    ok = ok && n == 1 && (out[0].events & EPOLLIN) && out[0].data.u64 == 0x1122334455667788ull;
    close(p[0]);
    close(p[1]);
  }
  check("epoll_ctl / epoll_wait (arm64 struct epoll_event)", ok);
  if (ep >= 0) close(ep);
}

// --- syscall, getauxval ---------------------------------------------------------------
static void test_syscall_auxv(void) {
  check("syscall(SYS_gettid) == gettid()", syscall(SYS_gettid) == gettid());
  int word = 0;
  check("syscall(SYS_futex, FUTEX_WAKE) == 0", syscall(SYS_futex, &word, 1 /* WAKE */, 1) == 0);
  errno = 0;
  check("syscall of an unknown number fails with ENOSYS",
        syscall(4000) == -1 && errno == ENOSYS);
  unsigned long hw = getauxval(AT_HWCAP);
  check("getauxval(AT_HWCAP): FP, ASIMD; no LSE atomics",
        (hw & 3) == 3 && (hw & (1ul << 8)) == 0);
}

// --- atomics ---------------------------------------------------------------------------
static atomic_int g_counter;
static atomic_llong g_counter64;

static void* atomic_thread(void* arg) {
  (void)arg;
  for (int i = 0; i < 100000; ++i) {
    atomic_fetch_add(&g_counter, 1);
    atomic_fetch_add_explicit(&g_counter64, 2, memory_order_relaxed);
  }
  return NULL;
}

static void test_atomics(void) {
  pthread_t t[4];
  for (int i = 0; i < 4; ++i) pthread_create(&t[i], NULL, atomic_thread, NULL);
  for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);
  check("atomic_fetch_add from 4 threads", atomic_load(&g_counter) == 400000 &&
                                               atomic_load(&g_counter64) == 800000);
  int expected = 400000;
  check("atomic_compare_exchange_strong",
        atomic_compare_exchange_strong(&g_counter, &expected, 1) && atomic_load(&g_counter) == 1);
}

// --- va_list -----------------------------------------------------------------------------
static int __attribute__((noinline)) fmt3(char* buf, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

static char g_fmt1_buf[256];
// One fixed argument: seven integer arguments remain in registers.
static int __attribute__((noinline)) fmt1(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(g_fmt1_buf, sizeof(g_fmt1_buf), fmt, ap);
  va_end(ap);
  return r;
}

static void test_va_list(void) {
  char buf[256];
  fmt3(buf, sizeof(buf), "%d %s %ld %.2f", 42, "str", 1234567890123L, 2.5);
  check("vsnprintf through a guest va_list", strcmp(buf, "42 str 1234567890123 2.50") == 0);
  fmt3(buf, sizeof(buf), "%d %d %d %d %d %d %d %d|%.1f %.1f %.1f", 1, 2, 3, 4, 5, 6, 7, 8, 1.5,
       2.5, 3.5);
  int ok = strcmp(buf, "1 2 3 4 5 6 7 8|1.5 2.5 3.5") == 0;
  if (!ok) printf("    got '%s'\n", buf);
  check("vsnprintf: register and stack arguments, ints and doubles", ok);
  fmt1("%d %d %d %d %d %d %d %d %d", 11, 12, 13, 14, 15, 16, 17, 18, 19);
  ok = strcmp(g_fmt1_buf, "11 12 13 14 15 16 17 18 19") == 0;
  if (!ok) printf("    got '%s'\n", g_fmt1_buf);
  check("vsnprintf: seven integer registers left in the va_list", ok);
}

// --- a host function pointer called directly ---------------------------------------------------
static void test_host_pointer(void) {
  size_t (*fn)(const char*) = (size_t(*)(const char*))dlsym(RTLD_DEFAULT, "strlen");
  check("dlsym(RTLD_DEFAULT, \"strlen\")", fn != NULL);
  check("calling the host pointer from guest code", fn != NULL && fn("hello, host") == 11);
  int (*st)(const char*, struct stat*) = (int (*)(const char*, struct stat*))dlsym(RTLD_DEFAULT, "stat");
  struct stat s;
  check("dlsym(\"stat\") gives the guest-layout stat",
        st != NULL && st(".", &s) == 0 && S_ISDIR(s.st_mode));
}

// --- the stack protector ------------------------------------------------------------------------
static int __attribute__((noinline)) protected_frame(const char* s) {
  char buf[32];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  return (int)strlen(buf);
}

int gesher_selftest(const char* dir);
int gesher_selftest(const char* dir) {
  failures = 0;
  printf("arm64_abi_test:\n");
  check("stack protector frames return", protected_frame("guarded") == 7);
  test_tls();
  test_setjmp();
  test_stat_epoll(dir != NULL ? dir : ".");
  test_syscall_auxv();
  test_atomics();
  test_va_list();
  test_host_pointer();
  printf(failures == 0 ? "arm64_abi_test: all passed\n" : "arm64_abi_test: %d failed\n", failures);
  fflush(stdout);
  return failures;
}

uint64_t gesher_perf_kernel(uint32_t rounds);
uint64_t gesher_perf_kernel(uint32_t rounds) { return gesher_perf_kernel_impl(rounds); }
