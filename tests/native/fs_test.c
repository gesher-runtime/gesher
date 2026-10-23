// An x86-64 Android shared object built the way the NDK builds app code by
// default: -fstack-protector-strong (the guard at %fs:0x28 in every function
// with a local array) and thread-locals. It checks the ELF loader's %fs
// rewrite (compat/elf/fs_rewrite.h, docs/decisions/0014) from the inside:
// the guard and the thread pointer stay valid across kernel entry (sleeps)
// and differ per thread, thread-locals keep per-thread values across sleeps
// and threads, and new threads see the initial values.
//
// native-libs.yml builds it twice:
//   fs_test26.so  API 26: thread-locals are emulated (__emutls_get_address on
//                 the shim's pthread keys); %fs is the guard and inline asm.
//   fs_test29.so  API 29: ELF TLS. Default (general-dynamic, __tls_get_addr),
//                 local-dynamic and initial-exec variables, so the code has
//                 %fs:0 and %fs:(%reg) forms; and it dlopen()s fs_late.so
//                 while a thread is running, whose TLS then has to appear in
//                 that thread's existing block.
//
// elf_load_test calls gesher_set_search_dir() with the directory holding
// fs_late.so, then gesher_selftest(), which returns the number of failures.

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(const char* what, int ok) {
  printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

// Raw %fs reads, as bionic's own inline code and hand-written assembly do.
static uintptr_t read_guard(void) {
  uintptr_t v;
  __asm__ __volatile__("movq %%fs:0x28, %0" : "=r"(v));
  return v;
}
static uintptr_t read_tp(void) {
  uintptr_t v;
  __asm__ __volatile__("movq %%fs:0, %0" : "=r"(v));
  return v;
}

#if __ANDROID_API__ >= 29
#define ELF_TLS 1
__thread int gd_value = 5;                                        // general-dynamic
static __thread long ld_value = 6;                                // local-dynamic
__thread int ie_value __attribute__((tls_model("initial-exec"))) = 7;
__thread char ie_buf[48] __attribute__((tls_model("initial-exec")));
// An initial-exec load written out, with a destination other than the base
// register (the compiler's own `return ie_value` usually reuses it).
static int ie_load_asm(void) {
  int v;
  __asm__ __volatile__(
      "movq ie_value@GOTTPOFF(%%rip), %%rcx\n\t"
      "movl %%fs:(%%rcx), %0"
      : "=r"(v)
      :
      : "rcx");
  return v;
}
#else
#define ELF_TLS 0
__thread int gd_value = 5;  // emulated TLS below API 29
static __thread long ld_value = 6;
__thread int ie_value = 7;
__thread char ie_buf[48];
static int ie_load_asm(void) { return ie_value; }
#endif

// Which rewrites elf_load_test should have seen in this object: 1 stack
// guard, 2 thread pointer, 4 out-of-line (register-based) accesses.
int gesher_fs_expect(void) { return ELF_TLS ? 1 | 2 | 4 : 1 | 2; }

static char g_dir[512];
void gesher_set_search_dir(const char* dir) {
  snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : ".");
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

// Stack-protected (a local array) and sleeping between prologue and epilogue:
// the epilogue's guard read happens after kernel entry.
__attribute__((noinline)) static int guarded_sleep(int n) {
  char buf[64];
  snprintf(buf, sizeof(buf), "value-%d", n);
  sleep_ms(2);
  return (int)strlen(buf);
}

// --- per-thread checks -------------------------------------------------------

struct ThreadResult {
  int id;
  int ok_initial;  // saw the initial values
  int ok_kept;     // kept its own values across sleeps
  int ok_guard;    // guard non-zero and stable across sleeps
  int ok_tp;       // thread pointer stable, &ie_value below it
  uintptr_t guard, tp;
};

static void* worker(void* arg) {
  struct ThreadResult* r = arg;
  r->ok_initial = gd_value == 5 && ld_value == 6 && ie_value == 7 && ie_buf[0] == 0;
  r->guard = read_guard();
  r->tp = read_tp();
  int ok_kept = 1, ok_guard = r->guard != 0, ok_tp = r->tp != 0;
  for (int i = 0; i < 5; ++i) {
    gd_value = r->id * 100 + i;
    ld_value = r->id * 1000 + i;
    ie_value = r->id * 10 + i;
    snprintf(ie_buf, sizeof(ie_buf), "thread %d round %d", r->id, i);
    ok_kept &= guarded_sleep(r->id) > 0;
    sleep_ms(1);
    char want[48];
    snprintf(want, sizeof(want), "thread %d round %d", r->id, i);
    ok_kept &= gd_value == r->id * 100 + i && ld_value == r->id * 1000 + i &&
               ie_value == r->id * 10 + i && ie_load_asm() == r->id * 10 + i &&
               strcmp(ie_buf, want) == 0;
    ok_guard &= read_guard() == r->guard;
    ok_tp &= read_tp() == r->tp;
  }
#if ELF_TLS
  // Static TLS sits below the thread pointer, in this thread's block, for
  // general-dynamic and initial-exec alike.
  uintptr_t ie = (uintptr_t)&ie_value, gd = (uintptr_t)&gd_value;
  ok_tp &= ie < r->tp && r->tp - ie < (1 << 20) && gd < r->tp && r->tp - gd < (1 << 20);
#endif
  r->ok_kept = ok_kept;
  r->ok_guard = ok_guard;
  r->ok_tp = ok_tp;
  return NULL;
}

static void test_threads(void) {
  enum { kThreads = 4 };
  pthread_t t[kThreads];
  struct ThreadResult r[kThreads];
  memset(r, 0, sizeof(r));
  for (int i = 0; i < kThreads; ++i) {
    r[i].id = i + 1;
    pthread_create(&t[i], NULL, worker, &r[i]);
  }
  for (int i = 0; i < kThreads; ++i) pthread_join(t[i], NULL);
  int initial = 1, kept = 1, guard = 1, tp = 1, distinct = 1;
  for (int i = 0; i < kThreads; ++i) {
    initial &= r[i].ok_initial;
    kept &= r[i].ok_kept;
    guard &= r[i].ok_guard;
    tp &= r[i].ok_tp;
    for (int j = 0; j < i; ++j) distinct &= r[i].tp != r[j].tp && r[i].guard != r[j].guard;
  }
  check("threads: new threads see the TLS initial values", initial);
  check("threads: thread-locals kept across sleeps (4 threads)", kept);
  check("threads: stack guard non-zero, stable across sleeps", guard);
  check("threads: thread pointer stable; TLS in the thread's block", tp);
  check("threads: each thread has its own guard and block", distinct);
  check("main thread's thread-locals untouched by the workers",
        gd_value == 5 && ld_value == 6 && ie_value == 7);
}

// --- a TLS library loaded while a thread is running (API 29) ----------------

#if ELF_TLS
typedef int (*IntFn)(void);
typedef void (*SetFn)(int);
typedef long* (*AddrFn)(void);

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_stage = 0;  // 1: thread ready, 2: library loaded, 3: thread done
static void* g_late = NULL;
static int g_late_ok_initial = 0, g_late_ok_own = 0;

static void wait_stage(int s) {
  pthread_mutex_lock(&g_mu);
  while (g_stage < s) pthread_cond_wait(&g_cv, &g_mu);
  pthread_mutex_unlock(&g_mu);
}
static void set_stage(int s) {
  pthread_mutex_lock(&g_mu);
  g_stage = s;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
}

static void* late_worker(void* arg) {
  (void)arg;
  ie_value = 99;  // this thread's block exists before the library is loaded
  set_stage(1);
  wait_stage(2);
  IntFn get_ie = (IntFn)dlsym(g_late, "late_get_ie");
  SetFn set_ie = (SetFn)dlsym(g_late, "late_set_ie");
  AddrFn gd_addr = (AddrFn)dlsym(g_late, "late_gd_addr");
  if (get_ie && set_ie && gd_addr) {
    g_late_ok_initial = get_ie() == 42 && *gd_addr() == 1234;
    set_ie(500);
    *gd_addr() = 5000;
    sleep_ms(2);
    g_late_ok_own = get_ie() == 500 && *gd_addr() == 5000 && ie_value == 99;
  }
  set_stage(3);
  return NULL;
}

static void test_late_library(void) {
  pthread_t t;
  pthread_create(&t, NULL, late_worker, NULL);
  wait_stage(1);
  char path[600];
  snprintf(path, sizeof(path), "%s/fs_late.so", g_dir);
  g_late = dlopen(path, RTLD_NOW);
  if (g_late == NULL) printf("  dlopen(%s): %s\n", path, dlerror());
  check("dlopen fs_late.so while a thread is running", g_late != NULL);
  set_stage(2);
  pthread_join(t, NULL);
  check("late TLS: the running thread sees its initial values", g_late_ok_initial);
  check("late TLS: and keeps its own values", g_late_ok_own);
  if (g_late != NULL) {
    IntFn get_ie = (IntFn)dlsym(g_late, "late_get_ie");
    AddrFn gd_addr = (AddrFn)dlsym(g_late, "late_gd_addr");
    check("late TLS: the loading thread has its own copy",
          get_ie && gd_addr && get_ie() == 42 && *gd_addr() == 1234);
  }
}
#endif

int gesher_selftest(void) {
  printf("fs_test (API %d, %s TLS): %%fs rewritten by the ELF loader\n", __ANDROID_API__,
         ELF_TLS ? "ELF" : "emulated");
  uintptr_t g = read_guard(), tp = read_tp();
  check("stack guard (%fs:0x28) is non-zero", g != 0);
  check("thread pointer (%fs:0) is set and is its own TCB self slot",
        tp != 0 && *(uintptr_t*)tp == tp);
  check("a stack-protected function returns across a sleep", guarded_sleep(7) == 7);
  check("stack guard unchanged after the sleep", read_guard() == g);
  check("thread pointer unchanged after the sleep", read_tp() == tp);
  check("thread-locals start at their initial values",
        gd_value == 5 && ld_value == 6 && ie_value == 7 && ie_load_asm() == 7);
  ie_value = 70;
  gd_value = 50;
  sleep_ms(1);
  check("thread-locals keep values across a sleep",
        ie_value == 70 && ie_load_asm() == 70 && gd_value == 50);
  ie_value = 7;
  gd_value = 5;
  test_threads();
#if ELF_TLS
  test_late_library();
#endif
  if (failures == 0) {
    printf("fs_test: all passed\n");
  } else {
    printf("fs_test: %d check(s) failed\n", failures);
  }
  return failures;
}
