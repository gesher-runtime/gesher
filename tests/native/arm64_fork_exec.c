// fork/vfork/clone/wait in translated arm64 code: a real child process (ADR 0092).
// An arm64-v8a executable, run through gesher-exec (tracks/47-arm64.ps1).
//
// Each check prints PASS or FAIL; the last line is the summary.
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <linux/netlink.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>

#ifndef __WALL
#define __WALL 0x40000000
#endif
#ifndef __WCLONE
#define __WCLONE 0x80000000
#endif

extern char** environ;

static int failures;

static void check(int ok, const char* what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
  fflush(stdout);
}

static void checkf(int ok, const char* what, const char* fmt, long a, long b) {
  if (ok) {
    check(1, what);
    return;
  }
  printf("FAIL %s: ", what);
  printf(fmt, a, b);
  printf("\n");
  fflush(stdout);
  ++failures;
}

// Waits for `pid` and returns its status word, or -1.
static int reap(pid_t pid, int options) {
  int status = 0;
  pid_t got;
  do {
    got = waitpid(pid, &status, options);
  } while (got < 0 && errno == EINTR);
  return got == pid ? status : -1;
}

static long compute(long n) {
  long s = 0;
  for (long i = 1; i <= n; ++i) s += i * i % 7;
  return s;
}

// --- the child computes and exits with a code ---------------------------------
static void test_exit_code(void) {
  pid_t parent = getpid();
  int fds[2];
  if (pipe(fds) != 0) { check(0, "fork: pipe"); return; }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int ids[2] = {(int)getpid(), (int)getppid()};
    write(fds[1], ids, sizeof ids);
    _exit((int)(compute(1000) % 200));
  }
  close(fds[1]);
  checkf(pid > 0, "fork returns the child's pid in the parent", "pid=%ld errno=%ld", pid, errno);
  if (pid <= 0) { close(fds[0]); return; }
  int ids[2] = {0, 0};
  ssize_t n = read(fds[0], ids, sizeof ids);
  close(fds[0]);
  checkf(n == sizeof ids && ids[0] == pid, "child's getpid is the pid fork returned", "child=%ld pid=%ld", ids[0], pid);
  checkf(ids[1] == parent, "child's getppid is the parent", "ppid=%ld parent=%ld", ids[1], parent);
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == (int)(compute(1000) % 200),
         "waitpid sees the child's computed exit code", "status=0x%lx want=%ld", status,
         compute(1000) % 200);
  int again = waitpid(pid, &status, 0);
  checkf(again == -1 && errno == ECHILD, "a reaped child is not waited for again", "r=%ld errno=%ld", again, errno);
}

// --- copy-on-write ---------------------------------------------------------------
static volatile int g_value = 1;

static void test_copy_on_write(void) {
  char* heap = malloc(4096);
  memset(heap, 'p', 4096);
  int local = 10;
  volatile int* plocal = &local;
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    g_value = 2;
    memset(heap, 'c', 4096);
    *plocal = 20;
    // The child sees its own writes.
    _exit(g_value == 2 && heap[100] == 'c' && *plocal == 20 ? 0 : 1);
  }
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "the child sees its own memory writes", "status=0x%lx %ld", status, 0);
  check(g_value == 1 && heap[100] == 'p' && heap[4095] == 'p' && local == 10,
        "the child's writes do not reach the parent (copy-on-write)");
  free(heap);
}

// --- a pipe between parent and child, both ways --------------------------------
static void test_pipe(void) {
  int up[2], down[2];
  if (pipe(up) != 0 || pipe(down) != 0) { check(0, "pipe"); return; }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(up[0]);
    close(down[1]);
    char buf[64] = {0};
    ssize_t n = read(down[0], buf, sizeof buf - 1);
    char reply[96];
    int len = snprintf(reply, sizeof reply, "child got <%.*s>", n > 0 ? (int)n : 0, buf);
    write(up[1], reply, (size_t)len);
    close(up[1]);
    // EOF once the parent closes its end.
    ssize_t eof = read(down[0], buf, sizeof buf);
    _exit(eof == 0 ? 0 : 1);
  }
  close(up[1]);
  close(down[0]);
  write(down[1], "ping", 4);
  char buf[96] = {0};
  ssize_t total = 0, n;
  while ((n = read(up[0], buf + total, sizeof buf - 1 - total)) > 0) total += n;
  close(down[1]);
  close(up[0]);
  check(strcmp(buf, "child got <ping>") == 0, "a pipe carries bytes parent -> child -> parent");
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "the child sees EOF when the parent closes its end", "status=0x%lx %ld", status, 0);
}

// --- a multithreaded parent forks while other threads run translated code -------
static atomic_int g_stop;
static atomic_long g_work;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_shared;

static void* worker(void* arg) {
  (void)arg;
  while (!atomic_load(&g_stop)) {
    void* p = malloc(64 + (size_t)(atomic_load(&g_work) & 255));
    pthread_mutex_lock(&g_mutex);
    g_shared += compute(50);
    pthread_mutex_unlock(&g_mutex);
    free(p);
    atomic_fetch_add(&g_work, 1);
  }
  return NULL;
}

// As POSIX requires of a multithreaded fork, a lock the child takes is held
// across the fork by pthread_atfork handlers (a lock another thread held at
// that moment would stay held in the child, on Linux too).
static void lock_shared(void) { pthread_mutex_lock(&g_mutex); }
static void unlock_shared(void) { pthread_mutex_unlock(&g_mutex); }

static void test_threads_stress(void) {
  enum { kThreads = 4, kForks = 120 };
  pthread_t threads[kThreads];
  atomic_store(&g_stop, 0);
  pthread_atfork(lock_shared, unlock_shared, unlock_shared);
  for (int i = 0; i < kThreads; ++i) pthread_create(&threads[i], NULL, worker, NULL);
  int ok = 0;
  fflush(stdout);
  for (int i = 0; i < kForks; ++i) {
    pid_t pid = fork();
    if (pid == 0) {
      // malloc, a mutex the parent's threads use, and translated code.
      char* p = malloc(1000);
      pthread_mutex_lock(&g_mutex);
      long v = g_shared;
      pthread_mutex_unlock(&g_mutex);
      (void)v;
      memset(p, i, 1000);
      int code = (int)((compute(200 + i) + p[10]) & 0x7f);
      free(p);
      _exit(code);
    }
    if (pid < 0) break;
    int status = reap(pid, 0);
    char* q = malloc(1000);
    memset(q, i, 1000);
    int want = (int)((compute(200 + i) + q[10]) & 0x7f);
    free(q);
    if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == want) ++ok;
  }
  atomic_store(&g_stop, 1);
  for (int i = 0; i < kThreads; ++i) pthread_join(threads[i], NULL);
  checkf(ok == kForks, "120 forks while 4 threads run translated code, malloc and a mutex",
         "%ld of %ld children exited with their codes", ok, kForks);
  check(atomic_load(&g_work) > 0, "the parent's threads kept running");
}

// --- raw clone(220), wait4(260), exit(93), exit_group(94) ------------------------

// A raw `svc #0` clone with a child stack. The child runs on that stack: it
// exits (raw exit) with 0 if its sp is within [low, top], else 1.
static long svc_clone_on_stack(unsigned long flags, void* top, void* low, int* ptid) {
  register long x0 __asm__("x0") = (long)flags;
  register void* x1 __asm__("x1") = top;
  register int* x2 __asm__("x2") = ptid;
  register long x3 __asm__("x3") = 0;
  register long x4 __asm__("x4") = 0;
  register long x8 __asm__("x8") = __NR_clone;
  register void* x9 __asm__("x9") = low;
  __asm__ volatile(
      "svc #0; "
      "cbnz x0, 1f; "
      "mov x10, sp; "
      "mov x0, #1; "
      "cmp x10, x9; "
      "b.lo 2f; "
      "cmp x10, x1; "
      "b.hi 2f; "
      "mov x0, #0; "
      "2: mov x8, #93; "
      "svc #0; "
      "1:"
      : "+r"(x0)
      : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8), "r"(x9)
      : "x10", "memory", "cc");
  return x0;
}

static void test_raw_clone(void) {
  fflush(stdout);
  long pid = syscall(__NR_clone, SIGCHLD, 0, 0, 0, 0);
  if (pid == 0) syscall(__NR_exit, 7);
  checkf(pid > 0, "raw clone(SIGCHLD) makes a child", "r=%ld errno=%ld", pid, errno);
  int status = 0;
  long got = syscall(__NR_wait4, pid, &status, 0, NULL);
  checkf(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
         "raw wait4 gets raw exit(93)'s status", "got=%ld status=0x%lx", got, status);

  pid = syscall(__NR_clone, 0, 0, 0, 0, 0);
  if (pid == 0) syscall(__NR_exit_group, 9);
  checkf(pid > 0, "raw clone(flags 0) makes a child", "r=%ld errno=%ld", pid, errno);
  // Its exit signal is not SIGCHLD: a "clone" child.
  usleep(100 * 1000);
  got = waitpid((pid_t)pid, &status, WNOHANG);
  checkf(got == -1 && errno == ECHILD, "a clone child is not waited for without __WCLONE/__WALL",
         "got=%ld errno=%ld", got, errno);
  struct rusage usage;
  memset(&usage, 0xff, sizeof usage);
  got = syscall(__NR_wait4, pid, &status, __WALL, &usage);
  checkf(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 9,
         "wait4(__WALL) gets the clone child's exit_group status", "got=%ld status=0x%lx", got, status);
  check(usage.ru_utime.tv_usec >= 0 && usage.ru_utime.tv_usec < 1000000 && usage.ru_maxrss >= 0,
        "wait4 fills the child's rusage");

  pid = syscall(__NR_clone, 0, 0, 0, 0, 0);
  if (pid == 0) _exit(4);
  got = waitpid((pid_t)pid, &status, __WCLONE);
  checkf(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 4,
         "waitpid(__WCLONE) gets a clone child", "got=%ld status=0x%lx", got, status);

  // clone with a child stack: the child runs on it.
  // A child stack of its own: the top of a mapped region (16-byte aligned).
  enum { kStackBytes = 64 * 1024 };
  char* stack = mmap(NULL, kStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) { check(0, "mmap a child stack"); return; }
  char* stack_top = stack + kStackBytes;
  int ptid = 0;
  pid = svc_clone_on_stack(SIGCHLD | CLONE_PARENT_SETTID, stack_top, stack, &ptid);
  checkf(pid > 0, "raw svc clone with a child stack makes a child", "r=%ld %ld", pid, 0);
  checkf(ptid == pid, "CLONE_PARENT_SETTID stores the child's pid", "ptid=%ld pid=%ld", ptid, pid);
  status = reap((pid_t)pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "a clone child starts on the stack it was given", "status=0x%lx %ld", status, 0);

  // bionic's own fork: CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD.
  static int ctid = -1;
  long r = syscall(__NR_clone, CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD, 0, 0, 0, &ctid);
  if (r == 0) _exit(ctid == gettid() ? 0 : 1);
  status = reap((pid_t)r, 0);
  checkf(r > 0 && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "clone(CHILD_SETTID|CHILD_CLEARTID|SIGCHLD) stores the child's tid in its word",
         "r=%ld status=0x%lx", r, status);
  check(ctid == -1, "CHILD_SETTID writes only the child's copy of the word");
  r = syscall(__NR_clone, CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, stack_top, 0, 0, 0);
  checkf(r == -1 && errno == ENOSYS, "a thread-style raw clone keeps ENOSYS", "r=%ld errno=%ld", r, errno);
  munmap(stack, kStackBytes);
  r = syscall(__NR_clone, CLONE_SIGHAND | SIGCHLD, 0, 0, 0, 0);
  checkf(r == -1 && errno == EINVAL, "CLONE_SIGHAND without CLONE_VM is EINVAL", "r=%ld errno=%ld", r, errno);
  r = syscall(__NR_wait4, -1, &status, 0x100, NULL);
  checkf(r == -1 && errno == EINVAL, "wait4 with an unknown option is EINVAL", "r=%ld errno=%ld", r, errno);
  r = syscall(__NR_wait4, -1, &status, WNOHANG, NULL);
  checkf(r == -1 && errno == ECHILD, "wait4 with no children is ECHILD", "r=%ld errno=%ld", r, errno);
}

// --- vfork, WNOHANG, signals -----------------------------------------------------
static atomic_int g_sigchld;
static void on_sigchld(int sig) { (void)sig; atomic_fetch_add(&g_sigchld, 1); }

static void test_vfork_and_status(void) {
  fflush(stdout);
  pid_t pid = vfork();
  if (pid == 0) _exit(3);
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 3, "vfork's child exits with its code",
         "status=0x%lx %ld", status, 0);

  pid = fork();
  if (pid == 0) {
    usleep(300 * 1000);
    _exit(11);
  }
  int got = waitpid(pid, &status, WNOHANG);
  checkf(got == 0, "WNOHANG returns 0 while the child runs", "got=%ld %ld", got, 0);
  status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 11, "then a blocking wait gets it",
         "status=0x%lx %ld", status, 0);

  pid = fork();
  if (pid == 0) {
    kill(getpid(), SIGKILL);
    _exit(1);
  }
  status = reap(pid, 0);
  checkf(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "a child killed by SIGKILL is WIFSIGNALED", "status=0x%lx %ld", status, 0);

  pid = fork();
  if (pid == 0) {
    raise(SIGTERM);
    _exit(1);
  }
  status = reap(pid, 0);
  checkf(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
         "a child ended by SIGTERM's default action is WIFSIGNALED", "status=0x%lx %ld", status, 0);

  // The parent kills a running child.
  pid = fork();
  if (pid == 0) {
    for (;;) pause();
  }
  usleep(200 * 1000);
  kill(pid, SIGKILL);
  status = reap(pid, 0);
  checkf(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "the parent's kill(child, SIGKILL) ends it", "status=0x%lx %ld", status, 0);

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigchld;
  sigaction(SIGCHLD, &sa, NULL);
  atomic_store(&g_sigchld, 0);
  pid = fork();
  if (pid == 0) _exit(0);
  reap(pid, 0);
  for (int i = 0; i < 100 && atomic_load(&g_sigchld) == 0; ++i) usleep(10 * 1000);
  check(atomic_load(&g_sigchld) >= 1, "the parent gets SIGCHLD for a child's exit");
  signal(SIGCHLD, SIG_DFL);
}

// --- what Ijiami's check does (ADR 0086) -------------------------------------------
static void abort_handler(int sig) {
  (void)sig;
  syscall(__NR_kill, getpid(), SIGKILL);
}

static void test_ijiami_shape(void) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = abort_handler;
    sigaction(SIGABRT, &sa, NULL);
    syscall(__NR_kill, getpid(), SIGABRT);
    _exit(1);
  }
  int status = reap(pid, 0);
  checkf(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "a child's SIGABRT handler SIGKILLs it, in the child only", "status=0x%lx %ld", status, 0);

  pid = fork();
  if (pid == 0) {
    struct sigaction sa, libc_old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = abort_handler;
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGUSR2, NULL, &libc_old);
    struct {
      void* handler;
      unsigned long flags;
      void* restorer;
      unsigned long mask;
    } raw;
    memset(&raw, 0, sizeof raw);
    long r = syscall(__NR_rt_sigaction, SIGUSR2, NULL, &raw, 8);
    int same = r == 0 && raw.handler == (void*)libc_old.sa_handler;
    syscall(__NR_exit, same ? 0 : 2);
  }
  status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "sigaction and raw rt_sigaction agree in the child, which leaves by raw exit",
         "status=0x%lx %ld", status, 0);
  check(getpid() > 0, "the parent runs on after both children");
}

// --- pthread_atfork ------------------------------------------------------------------
static int g_prepare, g_parent, g_child;
static void on_prepare(void) { ++g_prepare; }
static void on_parent(void) { ++g_parent; }
static void on_child(void) { ++g_child; }

static void test_atfork(void) {
  pthread_atfork(on_prepare, on_parent, on_child);
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) _exit(g_prepare == 1 && g_child == 1 && g_parent == 0 ? 0 : 1);
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "pthread_atfork's prepare and child handlers run in the child", "status=0x%lx %ld", status, 0);
  check(g_prepare == 1 && g_parent == 1 && g_child == 0, "its prepare and parent handlers run in the parent");
}

// --- a child that forks ------------------------------------------------------------
static void test_grandchild(void) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    pid_t inner = fork();
    if (inner == 0) _exit(21);
    int status = 0;
    if (waitpid(inner, &status, 0) != inner || !WIFEXITED(status)) _exit(99);
    _exit(WEXITSTATUS(status) + 1);
  }
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 22,
         "a forked child forks and waits for its own child", "status=0x%lx %ld", status, 0);
}

// --- threads in the child, a fork from another thread, exit() and stdio ------------
static void* child_thread(void* arg) { return (void*)(compute((long)arg) % 100); }

static void* forking_thread(void* arg) {
  (void)arg;
  pid_t pid = fork();
  if (pid == 0) _exit(31);
  return (void*)(long)reap(pid, 0);
}

static void test_child_threads_and_exit(void) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    pthread_t t;
    void* result = NULL;
    if (pthread_create(&t, NULL, child_thread, (void*)400L) != 0) _exit(98);
    pthread_join(t, &result);
    _exit((int)(long)result == (int)(compute(400) % 100) ? 0 : 1);
  }
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "the child creates and joins a thread", "status=0x%lx %ld", status, 0);

  pthread_t t;
  void* result = NULL;
  pthread_create(&t, NULL, forking_thread, NULL);
  pthread_join(t, &result);
  status = (int)(long)result;
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 31,
         "a thread other than the main one forks and waits", "status=0x%lx %ld", status, 0);

  int fds[2];
  if (pipe(fds) != 0) { check(0, "pipe"); return; }
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], 1);
    printf("child stdio line\n");
    exit(12);  // flushes stdio, runs the exit handlers
  }
  close(fds[1]);
  char buf[64] = {0};
  ssize_t total = 0, n;
  while ((n = read(fds[0], buf + total, sizeof buf - 1 - total)) > 0) total += n;
  close(fds[0]);
  status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 12,
         "exit() in the child ends it with its status", "status=0x%lx %ld", status, 0);
  check(strcmp(buf, "child stdio line\n") == 0, "the child's stdio output reaches the pipe at exit()");
}

// --- stdio buffers, exit statuses, signals right after fork, autoreap, dns -----------

// A line the parent buffered (not written yet) must reach the output once:
// _exit, the raw exit and exit_group calls and a death by a signal end a
// child without flushing its copy of the buffer.
static void test_buffered_once(void) {
  int fds[2];
  if (pipe(fds) != 0) { check(0, "pipe"); return; }
  fflush(stdout);
  int saved = dup(1);
  dup2(fds[1], 1);
  close(fds[1]);
  setvbuf(stdout, NULL, _IOFBF, 4096);
  printf("BUFFERED-ONCE\n");
  for (int how = 0; how < 4; ++how) {
    pid_t pid = how == 0 ? fork() : (pid_t)syscall(__NR_clone, SIGCHLD, 0, 0, 0, 0);
    if (pid == 0) {
      if (how == 0) _exit(0);
      if (how == 1) syscall(__NR_exit, 0);
      if (how == 2) syscall(__NR_exit_group, 0);
      kill(getpid(), SIGKILL);
    }
    reap(pid, 0);
  }
  fflush(stdout);
  dup2(saved, 1);
  close(saved);
  char buf[256] = {0};
  ssize_t total = 0, n;
  while ((n = read(fds[0], buf + total, sizeof buf - 1 - total)) > 0) total += n;
  close(fds[0]);
  int count = 0;
  for (const char* p = buf; (p = strstr(p, "BUFFERED-ONCE")) != NULL; ++p) ++count;
  checkf(count == 1, "a line the parent buffered is written once (no child flushes its copy)",
         "seen %ld times %ld", count, 0);
}

static volatile sig_atomic_t g_race;
static void on_race(int sig) { (void)sig; g_race = 1; }

static void test_status_and_signals(void) {
  pid_t pid = fork();
  if (pid == 0) _exit(130);
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 130,
         "exit(130) is an exit, not a death by SIGINT", "status=0x%lx %ld", status, 0);
  pid = fork();
  if (pid == 0) raise(SIGINT);
  status = reap(pid, 0);
  checkf(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT,
         "a death by SIGINT is WIFSIGNALED", "status=0x%lx %ld", status, 0);

  // A signal sent the moment fork returns reaches the child's handler.
  int sigs[2] = {SIGUSR1, SIGURG};
  for (int k = 0; k < 2; ++k) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_race;
    sigaction(sigs[k], &sa, NULL);
    int handled = 0;
    for (int i = 0; i < 20; ++i) {
      g_race = 0;
      pid = fork();
      if (pid == 0) {
        for (int t = 0; t < 300 && !g_race; ++t) usleep(10000);
        _exit(g_race ? 0 : 7);
      }
      int r = kill(pid, sigs[k]);
      status = reap(pid, 0);
      if (r == 0 && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) ++handled;
    }
    signal(sigs[k], SIG_DFL);
    checkf(handled == 20, k == 0 ? "SIGUSR1 sent right after fork reaches the child's handler"
                                 : "SIGURG sent right after fork reaches the child's handler",
           "%ld of %ld", handled, 20);
  }

  // SIGCHLD ignored: children are reaped at their exit; wait then has none.
  signal(SIGCHLD, SIG_IGN);
  pid = fork();
  if (pid == 0) _exit(5);
  errno = 0;
  int got = waitpid(-1, &status, 0);
  checkf(got == -1 && errno == ECHILD, "with SIGCHLD ignored a child is reaped at its exit (ECHILD)",
         "got=%ld errno=%ld", got, errno);
  signal(SIGCHLD, SIG_DFL);

  // Name resolution in a child is an honest error, never a crash.
  pid = fork();
  if (pid == 0) {
    struct addrinfo* res = NULL;
    int r = getaddrinfo("localhost", "80", NULL, &res);
    _exit(r == EAI_SYSTEM ? 0 : r == 0 ? 2 : 3);
  }
  status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "getaddrinfo of a name in a child fails with EAI_SYSTEM", "status=0x%lx %ld", status, 0);
}

// --- a signal handled during wait: EINTR, or a restart with SA_RESTART --------
static volatile sig_atomic_t g_usr1_hits;
static void on_usr1(int sig) {
  (void)sig;
  ++g_usr1_hits;
}

static void test_wait_interrupted(void) {
  for (int restart = 0; restart <= 1; ++restart) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sa.sa_flags = restart ? SA_RESTART : 0;
    sigaction(SIGUSR1, &sa, &old);
    g_usr1_hits = 0;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      usleep(300000);
      kill(getppid(), SIGUSR1);
      usleep(700000);
      _exit(5);
    }
    int status = 0;
    pid_t got = waitpid(pid, &status, 0);
    const int error = errno;
    if (restart) {
      checkf(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 5 && g_usr1_hits == 1,
             "a handler with SA_RESTART runs during wait, and the wait goes on to the child's exit",
             "got=%ld hits=%ld", (long)got, (long)g_usr1_hits);
    } else {
      checkf(got == -1 && error == EINTR && g_usr1_hits == 1,
             "a handler without SA_RESTART interrupts wait with EINTR", "got=%ld errno=%ld", (long)got,
             (long)error);
      if (got != pid) reap(pid, 0);
    }
    sigaction(SIGUSR1, &old, NULL);
  }
}

// --- fork, then exec or posix_spawn in the child (the most common pattern) --------
// `self` is this program's path: it is run again in a small mode (see main).
static const char* g_self;

static void test_exec_from_child(void) {
  if (g_self == NULL) {
    check(1, "exec checks need the program's path (not given here)");
    return;
  }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    execl(g_self, "arm64-fork-exec", "exit130", (char*)NULL);
    _exit(97);
  }
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 130,
         "fork + execl: the parent sees the program's exit status", "status=0x%lx %ld", status, 0);

  pid = fork();
  if (pid == 0) {
    char* av[] = {(char*)"arm64-fork-exec", (char*)"selfkill", NULL};
    execve(g_self, av, environ);
    _exit(96);
  }
  status = reap(pid, 0);
  checkf(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
         "fork + execve: the program's death by SIGTERM is the child's", "status=0x%lx %ld", status, 0);

  pid = fork();
  if (pid == 0) {
    execl("/no/such/program", "x", (char*)NULL);
    _exit(errno == ENOENT ? 0 : 1);
  }
  status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "fork + execl of a missing program: ENOENT in the child", "status=0x%lx %ld", status, 0);

  int fds[2];
  if (pipe(fds) != 0) { check(0, "pipe"); return; }
  pid = fork();
  if (pid == 0) {
    close(fds[0]);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    char* av[] = {(char*)"arm64-fork-exec", (char*)"echo", NULL};
    pid_t spawned = 0;
    int r = posix_spawn(&spawned, g_self, &actions, NULL, av, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (r != 0) _exit(10);
    int st = 0;
    if (waitpid(spawned, &st, 0) != spawned) _exit(11);
    _exit(WIFEXITED(st) && WEXITSTATUS(st) == 7 ? 0 : 12);
  }
  close(fds[1]);
  char buf[64] = {0};
  ssize_t total = 0, n;
  while ((n = read(fds[0], buf + total, sizeof buf - 1 - total)) > 0) total += n;
  close(fds[0]);
  status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "fork + posix_spawn in the child: it starts the program and waits for it", "status=0x%lx %ld", status, 0);
  check(strstr(buf, "spawned-ok") != NULL, "the program the child spawned wrote to the child's pipe");
}

// --- a FIFO's descriptors across fork (ADR 0097) -----------------------------------
// The child's descriptors are the same open file descriptions: end of file
// comes only once every process closed its writer, EPIPE only once every
// process closed its reader. A child opens the FIFO anew, and a child execs
// a program with the FIFO as its standard output (the brokered spawn passes
// the FIFO's ring with the description).
static void test_fifo(void) {
  char path[64];
  snprintf(path, sizeof path, "arm64-fork-fifo-%d", (int)getpid());
  unlink(path);
  if (mkfifo(path, 0600) != 0) {
    checkf(0, "fifo: mkfifo", "errno=%ld %ld", errno, 0);
    return;
  }
  int reader = open(path, O_RDONLY | O_NONBLOCK);
  int writer = open(path, O_WRONLY);
  if (reader < 0 || writer < 0) {
    checkf(0, "fifo: open both ends", "reader=%ld writer=%ld", reader, writer);
    unlink(path);
    return;
  }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    // Writes through the inherited writer, then exits with this process's
    // copies closed: the parent's copies keep both ends open.
    _exit(write(writer, "abc", 3) == 3 ? 0 : 1);
  }
  int status = reap(pid, 0);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "fifo: a forked child writes through the inherited writer", "status=0x%lx %ld", status, 0);
  char buf[16] = {0};
  ssize_t n = read(reader, buf, sizeof buf);
  check(n == 3 && memcmp(buf, "abc", 3) == 0, "fifo: the parent reads the child's bytes");
  n = read(reader, buf, sizeof buf);
  checkf(n == -1 && errno == EAGAIN, "fifo: the parent's writer still open: EAGAIN, not end of file",
         "n=%ld errno=%ld", (long)n, errno);

  // The child holds the only writer once the parent closes its own: end of
  // file only after the child closes it too.
  int go[2];
  if (pipe(go) != 0) {
    check(0, "pipe");
    close(reader);
    close(writer);
    unlink(path);
    return;
  }
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    close(go[1]);
    char c;
    if (read(go[0], &c, 1) != 1) _exit(2);
    _exit(write(writer, "z", 1) == 1 ? 0 : 1);
  }
  close(go[0]);
  close(writer);
  usleep(100000);
  n = read(reader, buf, sizeof buf);
  checkf(n == -1 && errno == EAGAIN, "fifo: a writer open only in the child: no end of file yet",
         "n=%ld errno=%ld", (long)n, errno);
  write(go[1], "g", 1);
  close(go[1]);
  status = reap(pid, 0);
  n = read(reader, buf, sizeof buf);
  ssize_t eof = read(reader, buf + 1, sizeof buf - 1);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && n == 1 && buf[0] == 'z' && eof == 0,
         "fifo: the child's byte, then end of file once the child closed the last writer",
         "n=%ld eof=%ld", (long)n, (long)eof);

  // A writer whose only reader is a forked child's copy: EPIPE once the
  // child has closed it (exited).
  writer = open(path, O_WRONLY | O_NONBLOCK);
  check(writer >= 0, "fifo: open a writer while a reader exists");
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    char c[4];
    for (int i = 0; i < 200; ++i) {
      ssize_t got = read(reader, c, sizeof c);
      if (got > 0) _exit(c[0] == 'r' ? 0 : 1);
      usleep(10000);
    }
    _exit(3);
  }
  close(reader);
  n = write(writer, "r", 1);
  status = reap(pid, 0);
  checkf(n == 1 && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "fifo: the child reads through its copy of the only reader", "n=%ld status=0x%lx", (long)n, status);
  // A reader that ended without closing is counted out within the ring's
  // sweep interval (100 ms, ADR 0097), as for any exited peer.
  usleep(150000);
  signal(SIGPIPE, SIG_IGN);
  n = write(writer, "x", 1);
  checkf(n == -1 && errno == EPIPE, "fifo: EPIPE once the child closed the last reader",
         "n=%ld errno=%ld", (long)n, errno);
  signal(SIGPIPE, SIG_DFL);
  close(writer);

  // A child opens the FIFO by its path (a new description) and writes.
  reader = open(path, O_RDONLY | O_NONBLOCK);
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    int w = open(path, O_WRONLY);
    _exit(w >= 0 && write(w, "open", 4) == 4 ? 0 : 1);
  }
  status = reap(pid, 0);
  memset(buf, 0, sizeof buf);
  n = read(reader, buf, sizeof buf);
  checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && n == 4 && memcmp(buf, "open", 4) == 0,
         "fifo: a forked child opens the FIFO by its path and writes", "n=%ld status=0x%lx", (long)n, status);
  // A child execs a program with the FIFO as its standard output.
  if (g_self != NULL) {
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
      int w = open(path, O_WRONLY);
      if (w < 0 || dup2(w, 1) != 1) _exit(95);
      close(w);
      execl(g_self, "arm64-fork-exec", "echo", (char*)NULL);
      _exit(94);
    }
    char out[64] = {0};
    ssize_t total = 0;
    int saw_eof = 0;
    for (int i = 0; i < 1000; ++i) {
      n = read(reader, out + total, sizeof out - 1 - total);
      if (n > 0) total += n;
      else if (n == 0 && total > 0) { saw_eof = 1; break; }
      else usleep(10000);
    }
    status = reap(pid, 0);
    checkf(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 7 && strstr(out, "spawned-ok") != NULL &&
               saw_eof,
           "fifo: a forked child's exec'd program writes to the FIFO it inherited, then end of file",
           "status=0x%lx total=%ld", status, (long)total);
  }
  close(reader);
  unlink(path);
}

static int netlink_request_ack(int fd, unsigned seq) {
  struct nlmsghdr request = {
    .nlmsg_len = NLMSG_LENGTH(0),
    .nlmsg_type = NLMSG_NOOP,
    .nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK,
    .nlmsg_seq = seq,
  };
  return send(fd, &request, sizeof(request), 0) == sizeof(request);
}

static int netlink_read_ack(int fd, unsigned seq) {
  struct {
    struct nlmsghdr header;
    struct nlmsgerr error;
  } response;
  return recv(fd, &response, sizeof(response), MSG_DONTWAIT) == sizeof(response) &&
         response.header.nlmsg_type == NLMSG_ERROR && response.header.nlmsg_seq == seq &&
         response.error.error == 0;
}

static void test_netlink_across_fork(void) {
  const int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (sock < 0) { check(0, "fork: open NETLINK_ROUTE"); return; }
  int ready[2];
  if (pipe(ready) != 0) {
    check(0, "fork: open pipe for the NETLINK_ROUTE test");
    close(sock);
    return;
  }
  check(netlink_request_ack(sock, 41), "fork: enqueue NETLINK_ROUTE reply in the parent");
  const pid_t pid = fork();
  if (pid == 0) {
    close(ready[0]);
    const int consumed = netlink_read_ack(sock, 41) &&
                         fcntl(sock, F_SETFL, O_NONBLOCK) == 0;
    if (!consumed || write(ready[1], "!", 1) != 1) _exit(1);
    close(ready[1]);
    usleep(50 * 1000);
    _exit(netlink_request_ack(sock, 42) && close(sock) == 0 ? 0 : 1);
  }
  close(ready[1]);
  int ok = 0;
  if (pid > 0) {
    char notified;
    struct pollfd pipe_poll = {ready[0], POLLIN, 0};
    const int child_read = poll(&pipe_poll, 1, 3000) == 1 &&
                           read(ready[0], &notified, 1) == 1 && notified == '!';
    struct pollfd socket_poll = {sock, POLLIN, 0};
    const int reply = child_read && poll(&socket_poll, 1, 3000) == 1 &&
                      (socket_poll.revents & POLLIN) && netlink_read_ack(sock, 42);
    int status = 0;
    if (!reply) kill(pid, SIGKILL);
    const int exited = waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                       WEXITSTATUS(status) == 0;
    ok = reply && exited && fcntl(sock, F_GETFL) == (O_RDWR | O_NONBLOCK) &&
         poll(&socket_poll, 1, 0) == 0;
  }
  check(ok, "NETLINK_ROUTE replies and file status are shared across guest fork");
  close(ready[0]);
  close(sock);
}

// --- a forked child whose root (the process that forked first) has ended ------
// The root (this program run again, in "killroot" mode) forks a child and
// ends; the child spawns: the spawn fails at once with ENOTSUP (ADR 0092: an
// orphaned clone cannot start processes) rather than waiting.
static void test_spawn_after_root_ends(void) {
  if (g_self == NULL) return;
  int fds[2];
  if (pipe(fds) != 0) { check(0, "pipe"); return; }
  char fd[16];
  snprintf(fd, sizeof fd, "%d", fds[1]);
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    execl(g_self, "arm64-fork-exec", "killroot", fd, g_self, (char*)NULL);
    _exit(97);
  }
  close(fds[1]);
  int status = reap(pid, 0);
  check(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the root ran and ended");
  struct pollfd p = {fds[0], POLLIN, 0};
  int result[2] = {-1, -1};
  const int ready = poll(&p, 1, 30000);
  const ssize_t n = ready == 1 ? read(fds[0], result, sizeof result) : -1;
  close(fds[0]);
  checkf(n == (ssize_t)sizeof result && result[0] == ENOTSUP && result[1] < 5000,
         "a forked child spawns after its root ended: ENOTSUP at once, no wait", "error=%ld ms=%ld",
         result[0], result[1]);
}

// --- a writable shared memfd mapping the child inherited ----------------------
// F_SEAL_WRITE fails with EBUSY while any writable shared mapping of the file
// exists, in any process: the child's inherited mapping counts once the
// parent has unmapped its own, and the seal succeeds after the child exits.
static void test_memfd_lease_in_child(void) {
  const int fd = (int)syscall(__NR_memfd_create, "fork-lease", MFD_ALLOW_SEALING);
  if (fd < 0) {
    check(0, "memfd_create");
    return;
  }
  if (ftruncate(fd, 4096) != 0) {
    check(0, "ftruncate");
    close(fd);
    return;
  }
  char* map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    check(0, "mmap");
    close(fd);
    return;
  }
  int ready[2], done[2];
  if (pipe(ready) != 0 || pipe(done) != 0) {
    check(0, "pipe");
    return;
  }
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(ready[0]);
    close(done[1]);
    map[0] = 'c';  // the child writes through its inherited mapping
    char c = 1;
    write(ready[1], &c, 1);
    read(done[0], &c, 1);  // until the parent has tried the seal
    _exit(0);
  }
  close(ready[1]);
  close(done[0]);
  char c;
  read(ready[0], &c, 1);
  munmap(map, 4096);
  errno = 0;
  const int busy = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
  const int busy_errno = errno;
  write(done[1], &c, 1);
  const int status = reap(pid, 0);
  const int sealed = fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE);
  checkf(busy == -1 && busy_errno == EBUSY, "F_SEAL_WRITE is EBUSY while the child maps the memfd writable",
         "r=%ld errno=%ld", busy, busy_errno);
  checkf(status != -1 && WIFEXITED(status) && sealed == 0,
         "F_SEAL_WRITE succeeds once the child has exited", "r=%ld status=0x%lx", sealed, status);
  close(ready[0]);
  close(done[1]);
  close(fd);
}

// The same without waiting for the child: the parent unmaps and seals right
// after fork returns. The child's mapping exists from the fork on, as on
// Linux, so the seal is EBUSY.
static void test_memfd_lease_at_once(void) {
  const int fd = (int)syscall(__NR_memfd_create, "fork-lease-now", MFD_ALLOW_SEALING);
  if (fd < 0 || ftruncate(fd, 4096) != 0) {
    check(0, "memfd_create");
    if (fd >= 0) close(fd);
    return;
  }
  char* map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    check(0, "mmap");
    close(fd);
    return;
  }
  int busy = 0, rounds = 10;
  for (int i = 0; i < rounds; ++i) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      usleep(200000);
      map[1] = 'c';
      _exit(0);
    }
    munmap(map, 4096);
    errno = 0;
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY) ++busy;
    reap(pid, 0);
    map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) break;  // sealed: no writable mapping any more
  }
  checkf(busy == rounds, "F_SEAL_WRITE right after fork is EBUSY while the child maps the memfd",
         "busy=%ld of %ld", busy, rounds);
  if (map != MAP_FAILED) munmap(map, 4096);
  close(fd);
}

// --- waits and the heap across the clone (ADR 0092) ---------------------------
//
// Waits for `pid` for at most `ms`; a child still running then is killed and
// -2 returned (a hang must fail the check, not the run).
static int reap_within(pid_t pid, int ms) {
  for (int waited = 0; waited < ms; waited += 5) {
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

// Threads of the parent wait on a condition variable (as a thread pool's
// workers do) when it forks. In the child a new thread waits on its copy and
// the only other thread signals it: the parent's waiters do not exist in the
// child, so the wake reaches the child's waiter, as on Linux.
static pthread_mutex_t g_cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static atomic_int g_cond_ready, g_cond_go, g_cond_done, g_cond_stop;

static void* parent_cond_waiter(void* arg) {
  (void)arg;
  pthread_mutex_lock(&g_cond_mutex);
  while (!atomic_load(&g_cond_stop)) pthread_cond_wait(&g_cond, &g_cond_mutex);
  pthread_mutex_unlock(&g_cond_mutex);
  return NULL;
}

static void* child_cond_waiter(void* arg) {
  (void)arg;
  pthread_mutex_lock(&g_cond_mutex);
  atomic_store(&g_cond_ready, 1);
  while (!atomic_load(&g_cond_go)) pthread_cond_wait(&g_cond, &g_cond_mutex);
  pthread_mutex_unlock(&g_cond_mutex);
  atomic_store(&g_cond_done, 1);
  return NULL;
}

static void test_condvar_waiters_at_fork(void) {
  enum { kWaiters = 4, kRounds = 20 };
  pthread_t waiters[kWaiters];
  for (int i = 0; i < kWaiters; ++i) pthread_create(&waiters[i], NULL, parent_cond_waiter, NULL);
  usleep(100000);  // they are waiting
  long woke = 0, lost = 0;
  for (int i = 0; i < kRounds; ++i) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      pthread_t t;
      if (pthread_create(&t, NULL, child_cond_waiter, NULL) != 0) _exit(11);
      for (int k = 0; k < 1000 && !atomic_load(&g_cond_ready); ++k) usleep(1000);
      usleep(20000);
      pthread_mutex_lock(&g_cond_mutex);
      atomic_store(&g_cond_go, 1);
      pthread_cond_signal(&g_cond);
      pthread_mutex_unlock(&g_cond_mutex);
      for (int k = 0; k < 3000 && !atomic_load(&g_cond_done); ++k) usleep(1000);
      _exit(atomic_load(&g_cond_done) ? 0 : 20);
    }
    const int status = pid > 0 ? reap_within(pid, 10000) : -1;
    if (status == 0) ++woke;
    else if (status != -1 && status != -2 && WIFEXITED(status) && WEXITSTATUS(status) == 20) ++lost;
  }
  atomic_store(&g_cond_stop, 1);
  pthread_mutex_lock(&g_cond_mutex);
  pthread_cond_broadcast(&g_cond);
  pthread_mutex_unlock(&g_cond_mutex);
  for (int i = 0; i < kWaiters; ++i) pthread_join(waiters[i], NULL);
  checkf(woke == kRounds,
         "a forked child's condition variable wakes its own waiter while the parent's threads waited on it",
         "woke=%ld lost=%ld", woke, lost);
}

// Threads of the parent allocate and free all the time while it forks (ntdll
// locks every heap for the clone, so they are waiting for the heap then); the
// child allocates on several threads of its own. No child may hang.
static atomic_int g_heap_stop;

static void* heap_churn(void* arg) {
  uint32_t seed = (uint32_t)(uintptr_t)arg * 2654435761u;
  void* slots[64] = {0};
  while (!atomic_load(&g_heap_stop)) {
    seed = seed * 1103515245u + 12345u;
    const unsigned i = (seed >> 8) % 64;
    free(slots[i]);
    slots[i] = malloc(16 + (seed >> 16) % 4096);
  }
  for (int i = 0; i < 64; ++i) free(slots[i]);
  return NULL;
}

static void* child_heap_work(void* arg) {
  uint32_t seed = (uint32_t)(uintptr_t)arg;
  for (int i = 0; i < 2000; ++i) {
    seed = seed * 1103515245u + 12345u;
    void* p = malloc(16 + (seed >> 16) % 4096);
    if (p == NULL) return (void*)1;
    memset(p, 1, 16);
    free(p);
  }
  return NULL;
}

static void test_heap_under_fork(void) {
  enum { kChurners = 4, kRounds = 20, kChildThreads = 4 };
  pthread_t churners[kChurners];
  atomic_store(&g_heap_stop, 0);
  for (int i = 0; i < kChurners; ++i) {
    pthread_create(&churners[i], NULL, heap_churn, (void*)(uintptr_t)(i + 1));
  }
  long ok = 0, hung = 0;
  for (int i = 0; i < kRounds; ++i) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      pthread_t t[kChildThreads];
      for (int k = 0; k < kChildThreads; ++k) {
        if (pthread_create(&t[k], NULL, child_heap_work, (void*)(uintptr_t)(k + 7)) != 0) _exit(11);
      }
      void* r = child_heap_work((void*)(uintptr_t)3);
      for (int k = 0; k < kChildThreads; ++k) {
        void* tr = NULL;
        if (pthread_join(t[k], &tr) != 0 || tr != NULL) _exit(12);
      }
      _exit(r == NULL ? 0 : 13);
    }
    const int status = pid > 0 ? reap_within(pid, 15000) : -1;
    if (status == 0) ++ok;
    else if (status == -2) ++hung;
  }
  atomic_store(&g_heap_stop, 1);
  for (int i = 0; i < kChurners; ++i) pthread_join(churners[i], NULL);
  checkf(ok == kRounds, "children allocate on several threads while the parent's threads churned the heap at the fork",
         "ok=%ld hung=%ld", ok, hung);
}

// --- ptrace across fork (ADR 0092, 0093) --------------------------------------

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

static long svc4(long nr, long a, long b, long c, long d) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  register long x4 __asm__("x4") = 0;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
  return x0;
}

// PairIP's sequence: the parent allows any tracer and clones itself with the
// raw system call (220); the child attaches to the parent, waits for the
// stop with wait4(__WALL), reads one of the parent's stack slots and writes
// the word into another, detaches and exits; the parent waits for the child.
static int pairip_child(pid_t parent, uintptr_t src, uintptr_t dst, uint64_t value) {
  if (ptrace(PTRACE_ATTACH, parent, 0, 0) != 0) return 2;
  int status = 0;
  if (svc4(__NR_wait4, parent, (long)&status, __WALL, 0) != parent) return 3;
  if (status != 0x137f) return 4;
  errno = 0;
  long word = ptrace(PTRACE_PEEKDATA, parent, (void*)src, 0);
  if (errno != 0 || (uint64_t)word != value) return 5;
  if (ptrace(PTRACE_POKEDATA, parent, (void*)dst, (void*)word) != 0) return 6;
  if (ptrace(PTRACE_DETACH, parent, 0, 0) != 0) return 7;
  return 0;
}

static void test_pairip_sequence(void) {
  check(prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == 0, "PairIP: PR_SET_DUMPABLE 1");
  check(prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) == 0, "PairIP: PR_SET_PTRACER_ANY");
  volatile uint64_t slots[2] = {0x5eed0f0e12345678ULL, 0};
  const pid_t parent = getpid();
  fflush(stdout);
  long pid = svc4(__NR_clone, SIGCHLD, 0, 0, 0);
  if (pid == 0) exit(pairip_child(parent, (uintptr_t)&slots[0], (uintptr_t)&slots[1], slots[0]));
  checkf(pid > 0, "PairIP: raw clone(220) makes the child", "r=%ld errno=%ld", pid, -pid);
  if (pid <= 0) return;
  const int status = reap_within((pid_t)pid, 30000);
  checkf(status == 0, "PairIP: the child attached, waited, peeked, poked, detached and exited 0",
         "status=0x%lx (child step %ld)", status, status > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  checkf(slots[1] == slots[0], "PairIP: the child's POKEDATA reached the parent's stack slot",
         "slot=0x%lx want=0x%lx", (long)slots[1], (long)slots[0]);
  prctl(PR_SET_PTRACER, 0, 0, 0, 0);
}

// A forked child keeps the dumpable flag (its mm's), and a tracer can stop
// it from the moment fork returns in it (its own ptrace state and guard).
static volatile uint64_t g_trace_marker;

static void test_trace_forked_child(void) {
  prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) _exit(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0));
  int status = pid > 0 ? reap_within(pid, 10000) : -1;
  prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
  checkf(status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "a forked child keeps the parent's dumpable flag (0)", "status=0x%lx (%ld)", status, 0);

  g_trace_marker = 1;
  fflush(stdout);
  pid = fork();
  if (pid == 0) {
    // Runs (with no Gesher wait of its own most of the time) until the parent
    // has written the marker.
    for (int i = 0; i < 20000 && g_trace_marker == 1; ++i) usleep(500);
    _exit(g_trace_marker == 0x7e57 ? 0 : 1);
  }
  if (pid <= 0) {
    check(0, "fork for the tracing check");
    return;
  }
  const long attach = ptrace(PTRACE_ATTACH, pid, 0, 0);
  int stop = 0;
  const pid_t got = attach == 0 ? waitpid(pid, &stop, __WALL) : -1;
  checkf(attach == 0 && got == pid && stop == 0x137f, "the parent attaches to its forked child and it stops",
         "attach=%ld stop=0x%lx", attach, stop);
  const long poke = ptrace(PTRACE_POKEDATA, pid, (void*)&g_trace_marker, (void*)0x7e57);
  const long detach = ptrace(PTRACE_DETACH, pid, 0, 0);
  status = reap_within(pid, 15000);
  checkf(poke == 0 && detach == 0 && status == 0,
         "POKEDATA into the stopped forked child, DETACH, and the child sees it", "poke=%ld status=0x%lx", poke,
         status);
}

// A forked tracee whose tracer seized it with PTRACE_O_EXITKILL dies by
// SIGKILL when the tracer exits (WIFSIGNALED, SIGKILL), as on Linux.
static void test_exitkill_forked_tracee(void) {
  int ready[2];
  if (pipe(ready) != 0) {
    check(0, "pipe for the EXITKILL check");
    return;
  }
  fflush(stdout);
  pid_t tracee = fork();
  if (tracee == 0) {
    close(ready[0]);
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    char c = 1;
    write(ready[1], &c, 1);
    for (int i = 0; i < 1000; ++i) usleep(10000);
    _exit(3);  // not killed: the check fails
  }
  close(ready[1]);
  char c = 0;
  const int got_ready = tracee > 0 && read(ready[0], &c, 1) == 1;
  close(ready[0]);
  if (!got_ready) {
    check(0, "the EXITKILL tracee started");
    if (tracee > 0) reap_within(tracee, 15000);
    return;
  }
  pid_t tracer = fork();
  if (tracer == 0) {
    // Seize it with EXITKILL and exit without detaching.
    _exit(ptrace(PTRACE_SEIZE, tracee, 0, (void*)(uintptr_t)PTRACE_O_EXITKILL) == 0 ? 0 : 1);
  }
  const int tracer_status = tracer > 0 ? reap_within(tracer, 15000) : -1;
  const int status = reap_within(tracee, 15000);
  checkf(tracer_status == 0 && status >= 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
         "a forked tracee dies by SIGKILL when its EXITKILL tracer exits", "tracer=0x%lx tracee=0x%lx",
         tracer_status, status);
}

static int run_all(void) {
  test_exit_code();
  test_copy_on_write();
  test_pipe();
  test_raw_clone();
  test_netlink_across_fork();
  test_vfork_and_status();
  test_ijiami_shape();
  test_atfork();
  test_grandchild();
  test_child_threads_and_exit();
  test_buffered_once();
  test_status_and_signals();
  test_exec_from_child();
  test_fifo();
  test_wait_interrupted();
  test_spawn_after_root_ends();
  test_memfd_lease_in_child();
  test_memfd_lease_at_once();
  test_condvar_waiters_at_fork();
  test_heap_under_fork();
  test_pairip_sequence();
  test_trace_forked_child();
  test_exitkill_forked_tracee();
  test_threads_stress();
  return failures;
}

#ifdef ARM64_FORK_JNI
// The same checks inside dalvikvm (ART's threads run while the guest forks):
// gesher.Arm64Fork loads this library and calls run().
#include <jni.h>
JNIEXPORT jint JNICALL Java_gesher_Arm64Fork_run(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  int failed = run_all();
  if (failed == 0) printf("arm64_fork_exec all passed\n");
  else printf("arm64_fork_exec: %d check(s) failed\n", failed);
  fflush(stdout);
  return failed;
}

// gesher.Arm64ForkChurn: `n` forks while Java threads keep starting and
// leaving (each one's exit callbacks run under ntdll's loader lock, which the
// clone waits for). Every child exits 0 within 10 s; returns the failures.
JNIEXPORT jint JNICALL Java_gesher_Arm64ForkChurn_forkLoop(JNIEnv* env, jclass cls, jint n) {
  (void)env;
  (void)cls;
  int fails = 0, hung = 0, bad = 0;
  for (int i = 0; i < n; ++i) {
    pid_t pid = fork();
    if (pid == 0) {
      void* p = malloc(64);
      free(p);
      _exit(getpid() > 0 ? 0 : 1);
    }
    if (pid < 0) {
      ++fails;
      continue;
    }
    int status = -2;
    for (int k = 0; k < 2000; ++k) {
      int st = 0;
      if (waitpid(pid, &st, WNOHANG) == pid) {
        status = st;
        break;
      }
      usleep(5000);
    }
    if (status == -2) {
      ++hung;
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    } else if (status != 0) {
      ++bad;
    }
  }
  printf("forkLoop n=%d fail=%d hung=%d bad=%d\n", n, fails, hung, bad);
  fflush(stdout);
  return fails + hung + bad;
}
#else
int main(int argc, char** argv) {
  // Small modes the exec checks run this program in.
  if (argc > 1 && strcmp(argv[1], "exit130") == 0) return 130;
  if (argc > 1 && strcmp(argv[1], "selfkill") == 0) {
    kill(getpid(), SIGTERM);
    sleep(5);
    return 99;
  }
  if (argc > 3 && strcmp(argv[1], "killroot") == 0) {
    // The root: a child that waits for it to end, then spawns.
    const int fd = atoi(argv[2]);
    pid_t child = fork();
    if (child == 0) {
      for (int i = 0; i < 1000 && getppid() != 1; ++i) usleep(10000);
      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      char* av[] = {(char*)"arm64-fork-exec", (char*)"echo", NULL};
      pid_t spawned = 0;
      const int r = posix_spawn(&spawned, argv[3], NULL, NULL, av, environ);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (r == 0) waitpid(spawned, NULL, 0);
      int result[2] = {r, (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000)};
      write(fd, result, sizeof result);
      _exit(0);
    }
    return child > 0 ? 0 : 1;
  }
  if (argc > 1 && strcmp(argv[1], "echo") == 0) {
    printf("spawned-ok\n");
    return 7;
  }
  // The checks: argv[1], when given, is this program's own path.
  if (argc > 1) g_self = argv[1];
  if (run_all() == 0) {
    printf("arm64_fork_exec all passed\n");
    return 0;
  }
  printf("arm64_fork_exec: %d check(s) failed\n", failures);
  return 1;
}
#endif
