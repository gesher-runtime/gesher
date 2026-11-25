// Adversarial checks for guest fork (ADR 0092), from an independent review.
// Usage: arm64-fork-adversarial <mode> [n] [case]
//   contend N [case]  N forks while three threads hammer one system call path
//                     (malloc, mmap, stdio, thread, env, prop, dlopen, exit,
//                     epoll); the child uses the same path. A child that hangs
//                     5 s or fails is counted. Exit 1 on any.
//   killrace N [sig]  a signal sent to a child right after fork reaches its
//                     handler (the child's mailbox exists before fork returns)
//   ctor LIB          a library constructor forks; parent and child use the
//                     loader afterwards
//   multi N, chain N, handler N, dupstdio K, exit130, cleartid, dns, timerfd
#include <android/dlext.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_stop;

static volatile sig_atomic_t g_race;
static void on_race_signal(int sig) { (void)sig; g_race = 1; }

static int reap(pid_t pid, int options) {
  int status = 0;
  pid_t got;
  do got = waitpid(pid, &status, options); while (got < 0 && errno == EINTR);
  return got == pid ? status : -1;
}

// Waits up to ms for pid; returns status, or -2 on timeout (then kills it).
static int reap_timeout(pid_t pid, int ms);

// --- a dl_iterate_phdr callback running on another thread at the fork --------
// The library is loaded into a reservation of the app's (android_dlopen_ext);
// another thread is inside a dl_iterate_phdr callback for it when the main
// thread forks. The child unmaps the reservation: that waits for callbacks on
// the object, and the other thread's never ends in the child (it does not
// exist there), so the child must not count it (ADR 0092).
static const char* g_cb_lib;
static volatile int g_in_cb;
static int cb_hold(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  (void)data;
  const char* base = strrchr(g_cb_lib, '/');
  base = base != NULL ? base + 1 : g_cb_lib;
  if (info->dlpi_name != NULL && strstr(info->dlpi_name, base) != NULL) {
    g_in_cb = 1;
    sleep(2);
    g_in_cb = 2;
  }
  return 0;
}
static void* cb_iterate(void* arg) {
  (void)arg;
  dl_iterate_phdr(cb_hold, NULL);
  return NULL;
}
// The forking thread itself 100 callbacks deep for the object: the child
// returns from all of them, then unmaps (its own callbacks are counted
// exactly, at any depth).
static int g_cb_depth;
static pid_t g_cb_child = -1;
static int cb_nest(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  (void)data;
  const char* base = strrchr(g_cb_lib, '/');
  base = base != NULL ? base + 1 : g_cb_lib;
  if (info->dlpi_name != NULL && strstr(info->dlpi_name, base) != NULL) {
    if (g_cb_depth < 100) {
      g_cb_depth++;
      dl_iterate_phdr(cb_nest, NULL);
      g_cb_depth--;
    } else if (g_cb_child == -1) {
      fflush(stdout);
      g_cb_child = fork();
    }
  }
  return 0;
}

static int cbfork(const char* lib) {
  g_cb_lib = lib;
  const size_t size = 8 << 20;
  void* reservation = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  android_dlextinfo ext;
  memset(&ext, 0, sizeof ext);
  ext.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
  ext.reserved_addr = reservation;
  ext.reserved_size = size;
  void* h = android_dlopen_ext(lib, RTLD_NOW, &ext);
  if (h == NULL) {
    printf("cbfork: dlopen failed: %s\n", dlerror());
    return 1;
  }
  pthread_t t;
  pthread_create(&t, NULL, cb_iterate, NULL);
  while (g_in_cb != 1) usleep(1000);
  fflush(stdout);
  pid_t c = fork();
  if (c == 0) {
    munmap(reservation, size);  // as an app does when it is done with it
    _exit(0);
  }
  const int st = reap_timeout(c, 15000);
  pthread_join(t, NULL);
  const int ok = st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
  printf("cbfork: the child unmapped the reservation: %s (status=0x%x)\n", ok ? "ok" : st == -2 ? "HUNG" : "FAILED",
         st);
  // Again, forking 100 callbacks deep on this thread.
  dl_iterate_phdr(cb_nest, NULL);
  if (g_cb_child == 0) {
    munmap(reservation, size);
    _exit(0);
  }
  const int deep = g_cb_child > 0 ? reap_timeout(g_cb_child, 15000) : -1;
  const int deep_ok = deep >= 0 && WIFEXITED(deep) && WEXITSTATUS(deep) == 0;
  printf("cbfork: forked 100 callbacks deep, the child unmapped the reservation: %s (status=0x%x)\n",
         deep_ok ? "ok" : deep == -2 ? "HUNG" : "FAILED", deep);
  return ok && deep_ok ? 0 : 1;
}

// Waits up to ms for pid; returns status, or -2 on timeout (then kills it).
static int reap_timeout(pid_t pid, int ms) {
  for (int i = 0; i < ms / 5; ++i) {
    int status = 0;
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) return status;
    if (got < 0 && errno != EINTR) return -1;
    usleep(5000);
  }
  kill(pid, SIGKILL);
  reap(pid, 0);
  return -2;
}

static void* t_mmap(void* a) {
  (void)a;
  while (!atomic_load(&g_stop)) {
    void* p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) munmap(p, 65536);
  }
  return NULL;
}
static void* t_stdio(void* a) {
  (void)a;
  FILE* f = fopen("/dev/null", "w");
  while (!atomic_load(&g_stop)) { fprintf(stderr, "%s", ""); if (f) fprintf(f, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx %d\n", 1); }
  return NULL;
}
static void* t_nop(void* a) { return a; }
static void* t_threads(void* a) {
  (void)a;
  while (!atomic_load(&g_stop)) { pthread_t t; if (pthread_create(&t, NULL, t_nop, NULL) == 0) pthread_join(t, NULL); }
  return NULL;
}
static void* t_env(void* a) {
  (void)a;
  int i = 0;
  while (!atomic_load(&g_stop)) { char v[16]; snprintf(v, sizeof v, "%d", i++); setenv("QA_X", v, 1); getenv("PATH"); }
  return NULL;
}
static void* t_prop(void* a) {
  (void)a;
  char v[PROP_VALUE_MAX];
  while (!atomic_load(&g_stop)) __system_property_get("ro.build.version.sdk", v);
  return NULL;
}
static void* t_atexit_fn_dummy(void* a) { return a; }
static void dummy_exit(void) {}
static void* t_atexit(void* a) {
  (void)a;
  int n = 0;
  while (!atomic_load(&g_stop) && n++ < 200000) atexit(dummy_exit);
  (void)t_atexit_fn_dummy;
  return NULL;
}
static void* t_dlopen(void* a) {
  (void)a;
  while (!atomic_load(&g_stop)) { void* h = dlopen("libm.so", RTLD_NOW); if (h) dlclose(h); }
  return NULL;
}
static void* t_epoll(void* a) {
  (void)a;
  while (!atomic_load(&g_stop)) {
    int ep = epoll_create1(0);
    int ev = eventfd(0, 0);
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = EPOLLIN;
    if (ep >= 0 && ev >= 0) epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
    if (ev >= 0) close(ev);
    if (ep >= 0) close(ep);
  }
  return NULL;
}
static void* t_malloc(void* a) {
  (void)a;
  while (!atomic_load(&g_stop)) { void* p = malloc(1 << (rand() % 20)); free(p); }
  return NULL;
}

typedef void* (*thread_fn)(void*);

// Child actions
static int child_action(const char* what) {
  if (!strcmp(what, "mmap")) {
    void* p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 3 : 0;
  }
  if (!strcmp(what, "stdio")) { fprintf(stderr, "%s", ""); fflush(stderr); return 0; }
  if (!strcmp(what, "thread")) { pthread_t t; if (pthread_create(&t, NULL, t_nop, NULL)) return 3; pthread_join(t, NULL); return 0; }
  if (!strcmp(what, "env")) { setenv("QA_Y", "1", 1); return getenv("QA_Y") ? 0 : 3; }
  if (!strcmp(what, "prop")) { char v[PROP_VALUE_MAX]; __system_property_get("ro.build.version.sdk", v); return 0; }
  if (!strcmp(what, "dlopen")) { void* h = dlopen("libm.so", RTLD_NOW); return h ? 0 : 3; }
  if (!strcmp(what, "malloc")) { void* p = malloc(100000); free(p); return 0; }
  if (!strcmp(what, "exit")) { exit(0); }
  if (!strcmp(what, "epoll")) {
    int ep = epoll_create1(0), ev = eventfd(1, 0);
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = EPOLLIN;
    if (ep < 0 || ev < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e) != 0) return 3;
    return epoll_wait(ep, &e, 1, 1000) == 1 ? 0 : 4;
  }
  return 0;
}

static int contend(thread_fn fn, const char* what, int forks) {
  pthread_t th[3];
  atomic_store(&g_stop, 0);
  for (int i = 0; i < 3; ++i) pthread_create(&th[i], NULL, fn, NULL);
  usleep(50000);
  int hung = 0, bad = 0, ok = 0;
  for (int i = 0; i < forks; ++i) {
    pid_t pid = fork();
    if (pid == 0) _exit(child_action(what));
    if (pid < 0) { printf("fork failed errno=%d\n", errno); bad++; break; }
    int st = reap_timeout(pid, 5000);
    if (st == -2) hung++;
    else if (st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++;
    else { bad++; printf("  status=0x%x\n", st); }
  }
  atomic_store(&g_stop, 1);
  for (int i = 0; i < 3; ++i) pthread_join(th[i], NULL);
  printf("contend %-7s: ok=%d hung=%d bad=%d of %d\n", what, ok, hung, bad, forks);
  fflush(stdout);
  return hung + bad;
}

static void on_alarm(int sig) {
  (void)sig;
  pid_t pid = fork();
  if (pid == 0) _exit(42);
  int st = reap(pid, 0);
  char b[64];
  int n = snprintf(b, sizeof b, "fork-in-handler status=0x%x\n", st);
  write(1, b, n);
}

static void* multi_forker(void* a) {
  long n = (long)a, ok = 0;
  for (long i = 0; i < n; ++i) {
    pid_t pid = fork();
    if (pid == 0) _exit((int)(i & 0x3f));
    int st = reap_timeout(pid, 10000);
    if (st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == (int)(i & 0x3f)) ok++;
  }
  return (void*)ok;
}

static int chain(int depth) {
  if (depth == 0) return 0;
  pid_t pid = fork();
  if (pid == 0) _exit(chain(depth - 1) == 0 ? 0 : 1);
  int st = reap_timeout(pid, 60000);
  return (st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
}

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "all";
  int n = argc > 2 ? atoi(argv[2]) : 50;
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (!strcmp(mode, "contend")) {
    const char* which = argc > 3 ? argv[3] : "all";
    struct { const char* name; thread_fn fn; } cases[] = {
        {"malloc", t_malloc}, {"mmap", t_mmap}, {"stdio", t_stdio}, {"thread", t_threads},
        {"env", t_env}, {"prop", t_prop}, {"dlopen", t_dlopen}, {"exit", t_atexit},
        {"epoll", t_epoll}};
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i)
      if (!strcmp(which, "all") || !strcmp(which, cases[i].name)) bad += contend(cases[i].fn, cases[i].name, n);
    return bad ? 1 : 0;
  }
  if (!strcmp(mode, "dupstdio")) {
    // stdout fully buffered (a file/pipe): a buffered line must not be printed twice.
    setvbuf(stdout, NULL, _IOFBF, 4096);
    printf("BUFFERED-ONCE\n");
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    reap(pid, 0);
    pid = syscall(__NR_clone, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) syscall(__NR_exit, 0);
    reap(pid, 0);
    pid = syscall(__NR_clone, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) syscall(__NR_exit_group, 0);
    reap(pid, 0);
    fflush(stdout);
    return 0;
  }
  if (!strcmp(mode, "exit130")) {
    pid_t pid = fork();
    if (pid == 0) _exit(130);
    int st = reap(pid, 0);
    printf("exit(130): WIFEXITED=%d WEXITSTATUS=%d WIFSIGNALED=%d WTERMSIG=%d\n", WIFEXITED(st),
           WEXITSTATUS(st), WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    pid = fork();
    if (pid == 0) _exit(137);
    st = reap(pid, 0);
    printf("exit(137): WIFEXITED=%d WIFSIGNALED=%d WTERMSIG=%d\n", WIFEXITED(st), WIFSIGNALED(st),
           WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return 0;
  }
  if (!strcmp(mode, "cleartid")) {
    // bionic's own fork() uses exactly these flags.
    int tid = 0;
    long r = syscall(__NR_clone, CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD, 0, 0, 0, &tid);
    if (r == 0) syscall(__NR_exit, 0);
    printf("clone(CHILD_SETTID|CHILD_CLEARTID|SIGCHLD): r=%ld errno=%d\n", r, r < 0 ? errno : 0);
    if (r > 0) reap(r, 0);
    r = syscall(__NR_clone, CLONE_VFORK | SIGCHLD, 0, 0, 0, 0);
    if (r == 0) syscall(__NR_exit, 0);
    printf("clone(CLONE_VFORK|SIGCHLD): r=%ld errno=%d\n", r, r < 0 ? errno : 0);
    r = syscall(__NR_clone, CLONE_VM | CLONE_VFORK | SIGCHLD, 0, 0, 0, 0);
    printf("clone(CLONE_VM|CLONE_VFORK|SIGCHLD): r=%ld errno=%d\n", r, r < 0 ? errno : 0);
    r = syscall(__NR_clone, CLONE_FILES | SIGCHLD, 0, 0, 0, 0);
    if (r == 0) syscall(__NR_exit, 0);
    printf("clone(CLONE_FILES|SIGCHLD): r=%ld errno=%d\n", r, r < 0 ? errno : 0);
    r = syscall(__NR_clone, CLONE_SETTLS | SIGCHLD, 0, 0, 0, 0);
    if (r == 0) syscall(__NR_exit, 0);
    printf("clone(CLONE_SETTLS|SIGCHLD): r=%ld errno=%d\n", r, r < 0 ? errno : 0);
    r = syscall(__NR_clone, 65 , 0, 0, 0, 0);
    printf("clone(sig 65): r=%ld errno=%d\n", r, r < 0 ? errno : 0);
    return 0;
  }
  if (!strcmp(mode, "handler")) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    for (int i = 0; i < n; ++i) { alarm(0); kill(getpid(), SIGALRM); usleep(20000); }
    raise(SIGALRM);
    usleep(200000);
    return 0;
  }
  if (!strcmp(mode, "multi")) {
    pthread_t th[4];
    for (int i = 0; i < 4; ++i) pthread_create(&th[i], NULL, multi_forker, (void*)(long)n);
    long total = 0;
    for (int i = 0; i < 4; ++i) { void* r; pthread_join(th[i], &r); total += (long)r; }
    printf("multi: %ld of %d ok\n", total, 4 * n);
    return total == 4 * n ? 0 : 1;
  }
  if (!strcmp(mode, "chain")) {
    int r = chain(n);
    printf("chain depth %d: %s\n", n, r == 0 ? "ok" : "FAILED");
    return r;
  }
  if (!strcmp(mode, "loop")) {
    // n forks, slow, for external handle-count sampling
    for (int i = 0; i < n; ++i) {
      pid_t pid = fork();
      if (pid == 0) _exit(0);
      reap(pid, 0);
      if (i % 100 == 0) { printf("iter %d\n", i); }
    }
    printf("loop done\n");
    sleep(3);
    return 0;
  }
  if (!strcmp(mode, "osync")) {
    const char* path = argc > 3 ? argv[3] : "osync.tmp";
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_SYNC, 0644);
    if (fd < 0) { printf("open failed %d\n", errno); return 1; }
    write(fd, "parent-before\n", 14);
    pid_t pid = fork();
    if (pid == 0) { write(fd, "child\n", 6); _exit(0); }
    int st = reap_timeout(pid, 10000);
    write(fd, "parent-after\n", 13);
    printf("osync child status=0x%x F_GETFL O_SYNC=%d\n", st, (fcntl(fd, F_GETFL) & O_SYNC) == O_SYNC);
    close(fd);
    return 0;
  }
  if (!strcmp(mode, "dns")) {
    pid_t pid = fork();
    if (pid == 0) {
      struct addrinfo* res = NULL;
      int r = getaddrinfo("localhost", "80", NULL, &res);
      _exit(r == 0 ? 0 : 10 + (r & 0xf));
    }
    int st = reap_timeout(pid, 15000);
    printf("dns in child: status=0x%x exited=%d code=%d signaled=%d sig=%d\n", st, WIFEXITED(st), WEXITSTATUS(st),
           WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return 0;
  }
  if (!strcmp(mode, "timerfd")) {
    pid_t pid = fork();
    if (pid == 0) {
      long r = syscall(__NR_timerfd_create, CLOCK_MONOTONIC, 0);
      _exit(r < 0 ? (errno & 0x7f) : 0);
    }
    int st = reap(pid, 0);
    printf("timerfd_create in child: errno-exit=%d\n", WEXITSTATUS(st));
    return 0;
  }
  if (!strcmp(mode, "killrace")) {
    int sig = argc > 3 ? atoi(argv[3]) : SIGUSR1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_race_signal;
    sigaction(sig, &sa, NULL);
    int handled = 0, killed = 0, lost = 0;
    for (int i = 0; i < n; ++i) {
      g_race = 0;
      pid_t pid = fork();
      if (pid == 0) {
        for (int k = 0; k < 300 && !g_race; ++k) usleep(10000);
        _exit(g_race ? 0 : 7);
      }
      int r = kill(pid, sig);
      int st = reap(pid, 0);
      if (r == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) handled++;
      else if (WIFSIGNALED(st)) killed++;
      else lost++;
    }
    printf("killrace sig %d: handled=%d killed=%d lost=%d of %d\n", sig, handled, killed, lost, n);
    return handled == n ? 0 : 1;
  }
  if (!strcmp(mode, "ctor")) {
    const char* lib = argc > 2 ? argv[2] : "libarm64forkctor.so";
    setenv("ARM64_FORK_CTOR", "1", 1);
    void* h = dlopen(lib, RTLD_NOW);
    unsetenv("ARM64_FORK_CTOR");
    if (!h) { printf("ctor: dlopen failed %s\n", dlerror()); return 1; }
    int* pid = (int*)dlsym(h, "g_ctor_pid");
    if (pid == NULL) { printf("ctor: no g_ctor_pid\n"); return 1; }
    if (*pid == 0) {
      void* m = dlopen("libm.so", RTLD_NOW);
      void* again = dlopen(lib, RTLD_NOW);
      _exit(m && again ? 0 : 5);
    }
    int st = reap_timeout(*pid, 10000);
    void* m = dlopen("libm.so", RTLD_NOW);
    int ok = *pid > 0 && st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && m != NULL;
    printf("ctor: fork in a constructor pid=%d child status=0x%x, loader usable after: %s\n", *pid, st,
           ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
  }
  if (!strcmp(mode, "cbfork")) return cbfork(argc > 2 ? argv[2] : "libarm64forkctor.so");
  printf("unknown mode\n");
  return 2;
}
