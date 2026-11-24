/* Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
/* ptrace(2) between real processes (ADR 0093), built with the NDK for x86_64
 * and arm64-v8a and run under gesher-exec. The program spawns copies of
 * itself (posix_spawn) in the roles below; every check is a real tracer
 * reading and writing a real tracee's memory.
 *
 *   ptrace-contract <self>                     the parent and the driver
 *   ptrace-contract <self> peekpoke <src> <dst> <value>
 *       attaches to its parent (allowed by PR_SET_PTRACER_ANY), waits for the
 *       stop, PEEKs <src>, POKEs the word into <dst>, then detaches
 *   ptrace-contract <self> attach-eperm <pid>  ATTACH must fail with EPERM
 *   ptrace-contract <self> attach-detach       attaches to its parent, waits
 *       for the stop and detaches
 *   ptrace-contract <self> attach-exit         attaches to its parent, waits
 *       for the stop and exits without detaching
 *   ptrace-contract <self> victim <fd>         publishes two addresses on <fd>
 *       and runs until its flag is set
 *   ptrace-contract <self> traceme             PTRACE_TRACEME once, then EPERM
 *   ptrace-contract <self> codevictim <fd>     publishes the addresses of code
 *       and read-only data a tracer patches
 *   ptrace-contract <self> lockvictim <fd> <file>  takes and releases a record
 *       lock in a loop (Gesher's cross-process locks while it is stopped)
 *   ptrace-contract <self> lockone <file>      one record lock (an unrelated process)
 *   ptrace-contract <self> spinvictim <fd>     as codevictim, waiting without host calls
 *   ptrace-contract <self> pipetracer <fd>     attaches to its parent, blocked reading a
 *       pipe, and writes into that pipe while the parent is stopped
 *   ptrace-contract <self> sleeper             sleeps 3 s
 *   ptrace-contract <self> sharedreader <fd> <pipe>  reads a pipe its parent shares
 *   ptrace-contract <self> stream <wfd> <rfd> <fd>  writes and reads large pipe streams
 *   ptrace-contract <self> bigwriter <wfd> <fd>  one 1 MB write() to a shared pipe
 *   ptrace-contract <self> [no]exitkill <fd>   seizes a child (PTRACE_O_EXITKILL
 *       or not) and exits
 * Lock files go next to the program.
 */
#define _GNU_SOURCE /* pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif
#ifndef PTRACE_EVENT_STOP
#define PTRACE_EVENT_STOP 128
#endif

extern char** environ;

static int failed;
static const char* self_path;

static void check(int okay, const char* name) {
  printf("%s  ptrace: %s\n", okay ? "PASS" : "FAIL", name);
  fflush(stdout);
  if (!okay) ++failed;
}

static pid_t spawn(char* const* argv) {
  pid_t pid = -1;
  int error = posix_spawn(&pid, self_path, NULL, NULL, argv, environ);
  if (error != 0) {
    printf("FAIL  ptrace: posix_spawn %s: %d\n", argv[2], error);
    return -1;
  }
  return pid;
}

/* The exit status of a child, or -1. */
static int wait_exit(pid_t pid) {
  int status = 0;
  if (waitpid(pid, &status, __WALL) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int tracer_pid(void) {
  FILE* f = fopen("/proc/self/status", "r");
  if (f == NULL) return -1;
  char line[256];
  int tracer = -1;
  while (fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, "TracerPid:", 10) == 0) tracer = atoi(line + 10);
  }
  fclose(f);
  return tracer;
}

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A raw system call, as packers and PairIP make them: -errno on failure. */
#if defined(__aarch64__)
#define NR_PTRACE 117
#define NR_WAIT4 260
static long raw_syscall4(long nr, long a, long b, long c, long d) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
  return x0;
}
#else
#define NR_PTRACE 101
#define NR_WAIT4 61
static long raw_syscall4(long nr, long a, long b, long c, long d) {
  long ret;
  register long r10 __asm__("r10") = d;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
                   : "rcx", "r11", "memory");
  return ret;
}
#endif

/* The directory of this program, for the lock files. */
static char work_dir[512];
static void lock_path(char* out, size_t size, const char* name) {
  snprintf(out, size, "%s/ptrace-%d-%s.lock", work_dir, (int)getpid(), name);
}

static int lock_once(const char* path) {
  int fd = open(path, O_RDWR | O_CREAT, 0600);
  if (fd < 0) return -1;
  struct flock fl = {0};
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_len = 1;
  int r = fcntl(fd, F_SETLK, &fl);
  fl.l_type = F_UNLCK;
  fcntl(fd, F_SETLK, &fl);
  close(fd);
  return r;
}

/* Code and read-only data a tracer patches. */
#if defined(__aarch64__)
__attribute__((noinline, naked)) int code_func(void) {
  __asm__ volatile("mov w0, #1\n ret\n nop\n nop\n");
}
static const uint64_t kPatchedCode = 0x52800040; /* mov w0, #2 (low word) */
static const uint64_t kCodeMask = 0xffffffffULL;
#else
__attribute__((noinline, naked)) int code_func(void) {
  __asm__ volatile("movl $1, %eax\n ret\n nop\n nop\n nop\n nop\n nop\n nop\n");
}
static const uint64_t kPatchedCode = 0x00000002b8ULL; /* movl $2, %eax */
static const uint64_t kCodeMask = 0xffffffffffULL;
#endif
static const volatile uint64_t ro_value = 0x1111222233334444ULL;

/* --- the children ------------------------------------------------------------ */

static int child_peekpoke(uintptr_t src, uintptr_t dst, uint64_t value) {
  pid_t parent = getppid();
  check(ptrace(PTRACE_ATTACH, parent, 0, 0) == 0, "child: ATTACH to the parent (PR_SET_PTRACER_ANY)");
  errno = 0;
  check(ptrace(PTRACE_ATTACH, parent, 0, 0) == -1 && errno == EPERM,
        "child: a second ATTACH by the same tracer is EPERM");
  int status = 0;
  check(raw_syscall4(NR_WAIT4, parent, (long)&status, __WALL, 0) == parent,
        "child: the raw wait4(parent, __WALL) system call reports the tracee");
  check(status == 0x137f && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
        "child: the attach stop is 0x137f (WIFSTOPPED, SIGSTOP)");
  check(waitpid(parent, &status, WNOHANG | __WALL) == 0, "child: the stop is reported once");

  errno = 0;
  long word = ptrace(PTRACE_PEEKDATA, parent, (void*)src, 0);
  check(errno == 0 && (uint64_t)word == value, "child: PEEKDATA reads the parent's stack slot");
  errno = 0;
  check(ptrace(PTRACE_PEEKTEXT, parent, (void*)src, 0) == (long)value && errno == 0,
        "child: PEEKTEXT reads the same word");
  uint64_t raw = 0;
  check(raw_syscall4(NR_PTRACE, PTRACE_PEEKDATA, parent, (long)src, (long)&raw) == 0 && raw == value,
        "child: the raw ptrace system call instruction (PEEKDATA) reads the slot");
  check(raw_syscall4(NR_PTRACE, PTRACE_PEEKDATA, parent, 8, (long)&raw) == -EIO,
        "child: the raw ptrace system call returns -EIO for an unmapped address");
  raw = 0;
  check(syscall(SYS_ptrace, PTRACE_PEEKDATA, parent, (void*)src, &raw) == 0 && raw == value,
        "child: the raw system call stores the word at data");
  errno = 0;
  check(syscall(SYS_ptrace, PTRACE_PEEKDATA, parent, (void*)src, (void*)8) == -1 && errno == EFAULT,
        "child: the raw system call with a bad data pointer is EFAULT");
  errno = 0;
  check(ptrace(PTRACE_PEEKDATA, parent, (void*)8, 0) == -1 && errno == EIO,
        "child: PEEKDATA of an unmapped address is EIO");
  errno = 0;
  check(ptrace(PTRACE_POKEDATA, parent, (void*)8, (void*)1) == -1 && errno == EIO,
        "child: POKEDATA to an unmapped address is EIO");
  check(ptrace(PTRACE_POKEDATA, parent, (void*)dst, (void*)word) == 0,
        "child: POKEDATA writes the word into the parent's other slot");
  check((uint64_t)ptrace(PTRACE_PEEKDATA, parent, (void*)dst, 0) == value,
        "child: the parent's slot reads back the poked word");
  errno = 0;
  {
    uint64_t regs[64];
    struct iovec io = {regs, sizeof regs};
    check(ptrace(PTRACE_GETREGSET, parent, (void*)1 /* NT_PRSTATUS */, &io) == -1 && errno == EIO,
          "child: registers are not provided (GETREGSET is EIO)");
  }

  /* A second tracer for the same tracee is refused, though it is allowed. */
  char pid_text[16];
  snprintf(pid_text, sizeof pid_text, "%d", (int)parent);
  char* argv[] = {"ptrace-contract", (char*)self_path, "attach-eperm", pid_text, NULL};
  pid_t other = spawn(argv);
  check(other > 0 && wait_exit(other) == 0, "child: another process's ATTACH to a traced process is EPERM");

  check(ptrace(PTRACE_DETACH, parent, 0, 0) == 0, "child: DETACH");
  errno = 0;
  check(ptrace(PTRACE_PEEKDATA, parent, (void*)src, 0) == -1 && errno == ESRCH,
        "child: after DETACH the parent is no tracee (ESRCH)");
  return failed == 0 ? 0 : 1;
}

static int child_attach_eperm(pid_t pid) {
  errno = 0;
  long r = ptrace(PTRACE_ATTACH, pid, 0, 0);
  if (r == -1 && errno == EPERM) return 0;
  printf("INFO  ptrace: attach-eperm got %ld errno %d\n", r, errno);
  if (r == 0) ptrace(PTRACE_DETACH, pid, 0, 0);
  return 1;
}

static int child_attach_exit(void) {
  pid_t parent = getppid();
  int status = 0;
  if (ptrace(PTRACE_ATTACH, parent, 0, 0) != 0) return 2;
  if (waitpid(parent, &status, __WALL) != parent || status != 0x137f) return 3;
  _exit(0); /* without PTRACE_DETACH */
}

static int child_attach_detach(void) {
  pid_t parent = getppid();
  int status = 0;
  if (ptrace(PTRACE_ATTACH, parent, 0, 0) != 0) return 2;
  if (waitpid(parent, &status, __WALL) != parent || status != 0x137f) return 3;
  return ptrace(PTRACE_DETACH, parent, 0, 0) == 0 ? 0 : 4;
}

static volatile uint64_t victim_value = 0x5a5aa5a5f00dfaceULL;
static volatile uint64_t victim_flag;

static int child_victim(int fd) {
  char text[64];
  int n = snprintf(text, sizeof text, "%" PRIxPTR " %" PRIxPTR "\n", (uintptr_t)&victim_value,
                   (uintptr_t)&victim_flag);
  if (write(fd, text, (size_t)n) != n) return 2;
  close(fd);
  for (int i = 0; i < 30000 && victim_flag == 0; ++i) sleep_ms(1);
  if (victim_flag != 0x600d) return 3;
  /* Its tracer is its parent while traced. */
  return tracer_pid() == getppid() ? 0 : 4;
}

static int child_traceme(void) {
  if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) return 2;
  errno = 0;
  if (ptrace(PTRACE_TRACEME, 0, 0, 0) != -1 || errno != EPERM) return 3;
  return tracer_pid() == getppid() ? 0 : 4;
}

/* Publishes its addresses on <fd>, then runs until the flag is set. */
static volatile uint64_t code_flag;
static int child_codevictim(int fd, int spin) {
  char text[128];
  int n = snprintf(text, sizeof text, "%" PRIxPTR " %" PRIxPTR " %" PRIxPTR "\n",
                   (uintptr_t)&code_func, (uintptr_t)&ro_value, (uintptr_t)&code_flag);
  if (write(fd, text, (size_t)n) != n) return 2;
  close(fd);
  int result = 0;
  if (spin) {
    /* No host call while it waits: translated code keeps running until the
     * tracer's write makes it return to the dispatcher (arm64). */
    while (code_flag == 0) result += code_func() - 1;
  } else {
    for (int i = 0; i < 30000 && code_flag == 0; ++i) {
      result = code_func(); /* translated (arm64) before the tracer patches it */
      sleep_ms(1);
    }
  }
  (void)result;
  /* 2 * 10: the patched code ran; + 1: the read-only word reads the poked value. */
  return code_func() * 10 + (ro_value == 0x5555666677778888ULL ? 1 : 0);
}

/* Attaches to its parent, one of whose threads is blocked reading a pipe,
 * waits (at most 5 s) for the stop, then writes the byte the parent waits for
 * into the pipe while the parent is stopped (the write end, inherited). */
static int child_pipetracer(int write_fd) {
  pid_t parent = getppid();
  if (ptrace(PTRACE_ATTACH, parent, 0, 0) != 0) return 2;
  int64_t t0 = now_ms();
  int status = 0, stopped = 0;
  while (now_ms() - t0 < 5000) {
    if (waitpid(parent, &status, WNOHANG | __WALL) == parent) {
      stopped = status == 0x137f;
      break;
    }
    sleep_ms(5);
  }
  printf("INFO  ptrace: pipe: stop after %d ms\n", (int)(now_ms() - t0));
  if (!stopped) return 3;
  int64_t w0 = now_ms();
  if (write(write_fd, "p", 1) != 1) return 4;
  if (now_ms() - w0 > 2000) return 5;
  return ptrace(PTRACE_DETACH, parent, 0, 0) == 0 ? 0 : 6;
}

/* Reads its pipe (shared with its parent) byte by byte, until killed. */
static int child_sharedreader(int fd, int report) {
  if (write(report, "ready\n", 6) != 6) return 2;
  close(report);
  for (;;) {
    char c;
    if (read(fd, &c, 1) <= 0) sleep_ms(1);
  }
}

static int child_sleeper(void) {
  sleep_ms(3000);
  return 0;
}

/* Holds and releases a record lock, as fast as it can, until killed. */
static const char* locker_path;
static void* locker(void* arg) {
  (void)arg;
  int fd = open(locker_path, O_RDWR | O_CREAT, 0600);
  if (fd < 0) return NULL;
  for (;;) {
    struct flock fl = {0};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_len = 1;
    fcntl(fd, F_SETLK, &fl);
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
  }
  return NULL;
}
static int child_lockvictim(int fd, const char* path) {
  locker_path = path;
  pthread_t t;
  pthread_create(&t, NULL, locker, NULL);
  if (write(fd, "ready\n", 6) != 6) return 2;
  close(fd);
  for (;;) sleep_ms(1000);
}

/* One record lock on a file of its own: an unrelated process. */
static int child_lockone(const char* path) { return lock_once(path) == 0 ? 0 : 1; }

/* Seizes its child with PTRACE_O_EXITKILL (or not), reports the child's pid
 * on <fd> and exits without detaching. */
static int child_exitkill_tracer(int fd, int exitkill) {
  int fds[2];
  if (pipe(fds) != 0) return 2;
  char fd_text[16];
  snprintf(fd_text, sizeof fd_text, "%d", fds[1]);
  char* args[] = {"ptrace-contract", (char*)self_path, "victim", fd_text, NULL};
  pid_t victim = spawn(args);
  close(fds[1]);
  char text[64] = {0};
  if (victim <= 0 || read(fds[0], text, sizeof text - 1) <= 0) return 3;
  close(fds[0]);
  if (ptrace(PTRACE_SEIZE, victim, 0, (void*)(long)(exitkill ? PTRACE_O_EXITKILL : 0)) != 0) return 4;
  char out[16];
  int n = snprintf(out, sizeof out, "%d\n", (int)victim);
  if (write(fd, out, (size_t)n) != n) return 5;
  _exit(0); /* without PTRACE_DETACH */
}

/* --- the parent -------------------------------------------------------------- */

static pid_t start_victim(uintptr_t* value, uintptr_t* flag) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  char fd_text[16];
  snprintf(fd_text, sizeof fd_text, "%d", fds[1]);
  char* argv[] = {"ptrace-contract", (char*)self_path, "victim", fd_text, NULL};
  pid_t pid = spawn(argv);
  close(fds[1]);
  char text[64] = {0};
  size_t got = 0;
  while (pid > 0 && got < sizeof text - 1) {
    ssize_t n = read(fds[0], text + got, sizeof text - 1 - got);
    if (n <= 0) break;
    got += (size_t)n;
    if (memchr(text, '\n', got) != NULL) break;
  }
  close(fds[0]);
  if (sscanf(text, "%" SCNxPTR " %" SCNxPTR, value, flag) != 2) return -1;
  return pid;
}

static void parent_seize_interrupt(void) {
  uintptr_t value = 0, flag = 0;
  pid_t victim = start_victim(&value, &flag);
  check(victim > 0, "seize: the victim child published its addresses");
  if (victim <= 0) return;
  errno = 0;
  check(ptrace(PTRACE_SEIZE, victim, (void*)1, 0) == -1 && errno == EIO,
        "seize: SEIZE with a nonzero addr is EIO");
  check(ptrace(PTRACE_SEIZE, victim, 0, 0) == 0, "seize: SEIZE does not stop the child");
  int status = 0;
  check(waitpid(victim, &status, WNOHANG | __WALL) == 0, "seize: no stop to report");
  errno = 0;
  check(ptrace(PTRACE_PEEKDATA, victim, (void*)value, 0) == -1 && errno == ESRCH,
        "seize: PEEK of a running seized tracee is ESRCH");
  check(ptrace(PTRACE_INTERRUPT, victim, 0, 0) == 0, "seize: INTERRUPT");
  check(waitpid(victim, &status, __WALL) == victim && WIFSTOPPED(status) &&
            WSTOPSIG(status) == SIGTRAP && (status >> 16) == PTRACE_EVENT_STOP,
        "seize: wait4 reports PTRACE_EVENT_STOP");
  check((uint64_t)ptrace(PTRACE_PEEKDATA, victim, (void*)value, 0) == 0x5a5aa5a5f00dfaceULL,
        "seize: PEEKDATA reads the child's value");
  check(ptrace(PTRACE_POKEDATA, victim, (void*)flag, (void*)0x600d) == 0,
        "seize: POKEDATA sets the child's flag");
  /* The child checks its TracerPid once it sees the flag, so stay its tracer
   * while it runs on: CONT, then wait for its exit. */
  check(ptrace(PTRACE_CONT, victim, 0, (void*)SIGTERM) == 0,
        "seize: CONT with SIGTERM (ignored at an event stop)");
  check(wait_exit(victim) == 0, "seize: the child saw the poked flag and TracerPid == its parent");
}

static void parent_attach_detach(void) {
  uintptr_t value = 0, flag = 0;
  pid_t victim = start_victim(&value, &flag);
  check(victim > 0, "attach: the victim child published its addresses");
  if (victim <= 0) return;
  check(ptrace(PTRACE_ATTACH, victim, 0, 0) == 0,
        "attach: ATTACH to its own child (the ancestor rule; no PR_SET_PTRACER)");
  int status = 0;
  check(waitpid(victim, &status, __WALL) == victim && status == 0x137f,
        "attach: wait4 reports the child's attach stop");
  check((uint64_t)ptrace(PTRACE_PEEKDATA, victim, (void*)value, 0) == 0x5a5aa5a5f00dfaceULL,
        "attach: PEEKDATA reads the child's value");
  errno = 0;
  check(ptrace(PTRACE_SEIZE, victim, 0, 0) == -1 && errno == EPERM,
        "attach: SEIZE of its own tracee is EPERM");
  errno = 0;
  check(ptrace(PTRACE_INTERRUPT, victim, 0, 0) == -1 && errno == EIO,
        "attach: INTERRUPT of an attached (not seized) tracee is EIO");
  check(ptrace(PTRACE_CONT, victim, 0, 0) == 0, "attach: CONT");
  errno = 0;
  check(ptrace(PTRACE_PEEKDATA, victim, (void*)value, 0) == -1 && errno == ESRCH,
        "attach: PEEK of a running tracee is ESRCH");
  errno = 0;
  check(ptrace(PTRACE_DETACH, victim, 0, 0) == -1 && errno == ESRCH,
        "attach: DETACH of a running tracee is ESRCH");
  check(ptrace(PTRACE_KILL, victim, 0, 0) == 0, "attach: KILL ends a running tracee");
  int killed = 0;
  check(waitpid(victim, &killed, __WALL) == victim && WIFSIGNALED(killed) &&
            WTERMSIG(killed) == SIGKILL,
        "attach: wait4 reports the tracee killed by SIGKILL");
}

/* Runs `argv` and reads one line it writes on the fd number its argv[3] names. */
static pid_t spawn_reporting(char** argv, char* line, size_t size) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  char fd_text[16];
  snprintf(fd_text, sizeof fd_text, "%d", fds[1]);
  argv[3] = fd_text;
  pid_t pid = spawn(argv);
  close(fds[1]);
  size_t got = 0;
  memset(line, 0, size);
  while (pid > 0 && got < size - 1) {
    ssize_t n = read(fds[0], line + got, size - 1 - got);
    if (n <= 0) break;
    got += (size_t)n;
    if (memchr(line, '\n', got) != NULL) break;
  }
  close(fds[0]);
  return pid;
}

/* uid: another app's process cannot be attached to, nor attach (no
 * CAP_SYS_PTRACE), even where Yama would allow it. */
static void parent_uid(void) {
  const char* uid_var = "GESHER_UID=10999";
  size_t count = 0;
  while (environ[count] != NULL) ++count;
  char** env = calloc(count + 2, sizeof(char*));
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    if (strncmp(environ[i], "GESHER_UID=", 11) != 0) env[used++] = environ[i];
  }
  env[used++] = (char*)uid_var;
  env[used] = NULL;
  char self_text[16];
  snprintf(self_text, sizeof self_text, "%d", (int)getpid());
  /* The parent allows any tracer (PR_SET_PTRACER_ANY) and is dumpable. */
  char* args[] = {"ptrace-contract", (char*)self_path, "attach-eperm", self_text, NULL};
  pid_t child = -1;
  check(posix_spawn(&child, self_path, NULL, NULL, args, env) == 0 && wait_exit(child) == 0,
        "uid: a process of another uid cannot attach (EPERM), though PR_SET_PTRACER_ANY allows any");
  /* Its own child of another uid: the ancestor rule does not help. */
  int fds[2];
  if (pipe(fds) == 0) {
    char fd_text[16];
    snprintf(fd_text, sizeof fd_text, "%d", fds[1]);
    char* vargs[] = {"ptrace-contract", (char*)self_path, "victim", fd_text, NULL};
    pid_t victim = -1;
    int spawned = posix_spawn(&victim, self_path, NULL, NULL, vargs, env) == 0;
    close(fds[1]);
    char text[64];
    if (spawned && read(fds[0], text, sizeof text) > 0) {
      errno = 0;
      check(ptrace(PTRACE_ATTACH, victim, 0, 0) == -1 && errno == EPERM,
            "uid: ATTACH to its own child of another uid is EPERM");
      errno = 0;
      check(ptrace(PTRACE_SEIZE, victim, 0, 0) == -1 && errno == EPERM,
            "uid: SEIZE of its own child of another uid is EPERM");
    } else {
      check(0, "uid: the child of another uid started");
    }
    close(fds[0]);
    if (victim > 0) {
      kill(victim, SIGKILL);
      wait_exit(victim);
    }
  }
  free(env);
}

/* A stopped tracee holds none of Gesher's cross-process locks: the tracer and
 * an unrelated process take record locks while it is stopped (qa.c). */
static volatile int helper_done;
static volatile int64_t helper_ms;
static char helper_path[600];
static void* helper(void* arg) {
  (void)arg;
  int64_t t0 = now_ms();
  lock_once(helper_path);
  helper_ms = now_ms() - t0;
  helper_done = 1;
  return NULL;
}
static void parent_locks(int cycles) {
  char victim_lock[600], unrelated_lock[600];
  lock_path(victim_lock, sizeof victim_lock, "victim");
  lock_path(helper_path, sizeof helper_path, "tracer");
  lock_path(unrelated_lock, sizeof unrelated_lock, "unrelated");
  char line[64];
  char* args[] = {"ptrace-contract", (char*)self_path, "lockvictim", NULL, victim_lock, NULL};
  pid_t victim = spawn_reporting(args, line, sizeof line);
  check(victim > 0 && strncmp(line, "ready", 5) == 0, "locks: the locking victim runs");
  if (victim <= 0) return;
  sleep_ms(200);
  int tracer_blocked = 0, unrelated_blocked = 0, stops = 0;
  int64_t worst = 0;
  for (int c = 0; c < cycles; ++c) {
    if (ptrace(PTRACE_ATTACH, victim, 0, 0) != 0) break;
    int status = 0;
    if (waitpid(victim, &status, __WALL) == victim && status == 0x137f) ++stops;
    helper_done = 0;
    pthread_t h;
    int64_t t0 = now_ms();
    pthread_create(&h, NULL, helper, NULL);
    /* A lock that waited for the stopped tracee never completes (it did,
     * before ADR 0093's safe points: seconds, or for good); a loaded machine
     * makes an ordinary one slow, so the bound is generous and the time
     * measured is the lock's own. */
    while (!helper_done && now_ms() - t0 < 10000) sleep_ms(1);
    if (!helper_done || helper_ms > 5000) ++tracer_blocked;
    if (helper_done && helper_ms > worst) worst = helper_ms;
    if (c % 5 == 0) {
      char* uargs[] = {"ptrace-contract", (char*)self_path, "lockone", unrelated_lock, NULL};
      pid_t other = spawn(uargs);
      int done = 0;
      int64_t u0 = now_ms();
      while (other > 0 && !done && now_ms() - u0 < 15000) {
        int st = 0;
        if (waitpid(other, &st, WNOHANG) == other) done = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        else sleep_ms(5);
      }
      if (!done) ++unrelated_blocked;
    }
    ptrace(PTRACE_DETACH, victim, 0, 0);
    pthread_join(h, NULL);
    sleep_ms(3);
  }
  printf("INFO  ptrace: locks: %d stops of %d; tracer lock waited at most %" PRId64 " ms\n", stops,
         cycles, worst);
  check(stops == cycles, "locks: every attach stop is reported");
  check(tracer_blocked == 0, "locks: the tracer's own record lock never waits for the stopped tracee");
  check(unrelated_blocked == 0, "locks: an unrelated process's record lock never waits for the stopped tracee");
  kill(victim, SIGKILL);
  wait_exit(victim);
  unlink(victim_lock);
  unlink(helper_path);
  unlink(unrelated_lock);
}

/* POKE writes read-only memory (FOLL_FORCE), and a translated tracee runs the
 * code a tracer wrote. */
static void parent_poke_code(const char* role, int request) {
  char line[128];
  char* args[] = {"ptrace-contract", (char*)self_path, (char*)role, NULL, NULL};
  printf("INFO  ptrace: poke: %s, %s\n", role, request == PTRACE_POKETEXT ? "POKETEXT" : "POKEDATA");
  pid_t victim = spawn_reporting(args, line, sizeof line);
  uintptr_t fn = 0, ro = 0, flag = 0;
  check(victim > 0 && sscanf(line, "%" SCNxPTR " %" SCNxPTR " %" SCNxPTR, &fn, &ro, &flag) == 3,
        "poke: the code victim published its addresses");
  if (victim <= 0) return;
  sleep_ms(100); /* it has run (and translated) code_func by now */
  check(ptrace(PTRACE_ATTACH, victim, 0, 0) == 0, "poke: ATTACH");
  int status = 0;
  check(waitpid(victim, &status, __WALL) == victim && status == 0x137f, "poke: stopped");
  errno = 0;
  uint64_t word = (uint64_t)ptrace(PTRACE_PEEKTEXT, victim, (void*)fn, 0);
  check(errno == 0, "poke: PEEKTEXT of the code");
  uint64_t patched = (word & ~kCodeMask) | kPatchedCode;
  check(ptrace(request, victim, (void*)fn, (void*)patched) == 0,
        "poke: a POKE into read-only code succeeds (FOLL_FORCE)");
  check((uint64_t)ptrace(PTRACE_PEEKTEXT, victim, (void*)fn, 0) == patched, "poke: the code reads back");
  check(ptrace(PTRACE_POKEDATA, victim, (void*)ro, (void*)0x5555666677778888ULL) == 0,
        "poke: POKEDATA into read-only data succeeds (FOLL_FORCE)");
  check(ptrace(PTRACE_POKEDATA, victim, (void*)flag, (void*)1) == 0, "poke: POKEDATA sets the flag");
  check(ptrace(PTRACE_DETACH, victim, 0, 0) == 0, "poke: DETACH");
  int code = wait_exit(victim);
  printf("INFO  ptrace: poke: victim exit %d\n", code);
  check(code == 21, "poke: the tracee ran the patched code and read the patched constant");
}

/* A thread blocked reading an empty pipe is at a safe point: the process
 * stops at once, and the pipe's other end works while it is stopped. */
static int pipe_fds[2];
static volatile int pipe_got;
static void* pipe_reader(void* arg) {
  (void)arg;
  char c = 0;
  if (read(pipe_fds[0], &c, 1) == 1 && c == 'p') pipe_got = 1;
  return NULL;
}
static void parent_pipe(void) {
  if (pipe(pipe_fds) != 0) {
    check(0, "pipe: pipe()");
    return;
  }
  pipe_got = 0;
  pthread_t reader;
  pthread_create(&reader, NULL, pipe_reader, NULL);
  sleep_ms(200); /* blocked in read() by now */
  char fd_text[16];
  snprintf(fd_text, sizeof fd_text, "%d", pipe_fds[1]);
  char* args[] = {"ptrace-contract", (char*)self_path, "pipetracer", fd_text, NULL};
  pid_t tracer = spawn(args);
  int code = tracer > 0 ? wait_exit(tracer) : -1;
  printf("INFO  ptrace: pipe: tracer exit %d\n", code);
  check(code == 0, "pipe: a tracee blocked reading a pipe stops; its writer is not held up");
  if (code != 0 && write(pipe_fds[1], "x", 1) != 1) check(0, "pipe: release the reader");
  pthread_join(reader, NULL);
  check(pipe_got == 1, "pipe: the reader gets the byte written while it was stopped");
  close(pipe_fds[0]);
  close(pipe_fds[1]);
}

/* A pipe description shared by the tracer and a stopped tracee blocked
 * reading it (fork and inheritance share descriptions): the stop interrupts
 * that read, as on Linux, so the tracer's poll, read and fcntl on the same
 * description work, and the tracer reads the byte it wrote (qa5 "shared"). */
static void parent_shared_pipe(void) {
  int p[2];
  if (pipe(p) != 0) {
    check(0, "shared pipe: pipe()");
    return;
  }
  char line[32];
  char fd_text[16];
  snprintf(fd_text, sizeof fd_text, "%d", p[0]);
  char* args[] = {"ptrace-contract", (char*)self_path, "sharedreader", NULL, fd_text, NULL};
  pid_t reader = spawn_reporting(args, line, sizeof line);
  check(reader > 0 && strncmp(line, "ready", 5) == 0, "shared pipe: the reader runs");
  if (reader <= 0) return;
  sleep_ms(200); /* blocked in read() by now */
  check(ptrace(PTRACE_ATTACH, reader, 0, 0) == 0, "shared pipe: ATTACH");
  int status = 0;
  int64_t t0 = now_ms();
  int stopped = 0;
  while (!stopped && now_ms() - t0 < 5000) {
    if (waitpid(reader, &status, WNOHANG | __WALL) == reader) stopped = status == 0x137f;
    else sleep_ms(2);
  }
  check(stopped, "shared pipe: the reader blocked in read() stops");
  check(write(p[1], "y", 1) == 1, "shared pipe: the tracer writes a byte");
  struct pollfd pf = {.fd = p[0], .events = POLLIN};
  t0 = now_ms();
  int ready = poll(&pf, 1, 2000);
  printf("INFO  ptrace: shared pipe: poll -> %d (errno %d) revents %#x in %d ms\n", ready,
         ready < 0 ? errno : 0, pf.revents, (int)(now_ms() - t0));
  check(ready == 1 && (pf.revents & POLLIN) != 0,
        "shared pipe: the tracer's poll sees the byte (the stopped reader did not take it)");
  int flags = fcntl(p[0], F_GETFL);
  check(flags >= 0 && fcntl(p[0], F_SETFL, flags | O_NONBLOCK) == 0,
        "shared pipe: F_SETFL O_NONBLOCK on the shared description");
  char c = 0;
  t0 = now_ms();
  ssize_t n = read(p[0], &c, 1);
  check(n == 1 && c == 'y' && now_ms() - t0 < 2000, "shared pipe: the tracer reads its byte back");
  fcntl(p[0], F_SETFL, flags);
  check(ptrace(PTRACE_DETACH, reader, 0, 0) == 0, "shared pipe: DETACH");
  /* The reader restarted its read: it takes the next byte. While it is
   * blocked there it holds the description's gate (poll on that description
   * then fails with EOPNOTSUPP, stopped or not), so look while it is
   * stopped again. */
  check(write(p[1], "z", 1) == 1, "shared pipe: another byte for the resumed reader");
  sleep_ms(300);
  int left = -1;
  if (ptrace(PTRACE_ATTACH, reader, 0, 0) == 0) {
    stopped = 0;
    t0 = now_ms();
    while (!stopped && now_ms() - t0 < 5000) {
      if (waitpid(reader, &status, WNOHANG | __WALL) == reader) stopped = status == 0x137f;
      else sleep_ms(2);
    }
    pf.revents = 0;
    left = stopped ? poll(&pf, 1, 0) : -1;
    ptrace(PTRACE_DETACH, reader, 0, 0);
  }
  check(left == 0, "shared pipe: the resumed reader's restarted read takes it");
  kill(reader, SIGKILL);
  wait_exit(reader);
  close(p[0]);
  close(p[1]);
}

/* A process that has just been started can be attached to at once (its
 * state and guard come up during its start-up). */
static void parent_attach_at_spawn(void) {
  int ok = 0;
  for (int i = 0; i < 10; ++i) {
    char* args[] = {"ptrace-contract", (char*)self_path, "sleeper", NULL};
    pid_t child = spawn(args);
    if (child <= 0) continue;
    errno = 0;
    if (ptrace(PTRACE_SEIZE, child, 0, 0) == 0) ++ok;
    else printf("INFO  ptrace: spawn: SEIZE errno %d\n", errno);
    kill(child, SIGKILL);
    wait_exit(child);
  }
  check(ok == 10, "spawn: SEIZE right after posix_spawn succeeds (10 of 10)");
}

/* --- byte-exact pipe streams across repeated stops (QA round 4, qa6) ---------
 * The tracee writes a large stream into pipe A (kept full by a slow drainer in
 * the tracer) in 300 KB write() calls, and reads a stream from pipe B in
 * 200 KB read() calls, while the tracer stops and resumes it again and again.
 * Every byte at stream offset k is stream_byte(k): a byte lost or written
 * twice is a mismatch. On Linux a stop never makes read()/write() fail (the
 * call goes on after it), and the byte counts agree. */
#define STREAM_WCHUNK 300000
#define STREAM_RCHUNK 200000
static inline unsigned char stream_byte(uint64_t k) {
  return (unsigned char)((k * 2654435761u) >> 11 ^ (k >> 16) ^ k);
}
static int s_wfd, s_rfd;
static volatile uint64_t s_written, s_read, s_rmismatch, s_eintr, s_errors;
static volatile int s_stop;
static void* stream_writer(void* arg) {
  (void)arg;
  unsigned char* buf = malloc(STREAM_WCHUNK);
  uint64_t off = 0;
  for (;;) {
    for (int i = 0; i < STREAM_WCHUNK; ++i) buf[i] = stream_byte(off + i);
    size_t done = 0;
    while (done < STREAM_WCHUNK) {
      ssize_t n = write(s_wfd, buf + done, STREAM_WCHUNK - done);
      if (n < 0) {
        if (errno == EINTR) { ++s_eintr; continue; }
        if (errno != EPIPE) ++s_errors;
        s_written = off + done;
        free(buf);
        return NULL;
      }
      done += (size_t)n;
    }
    off += STREAM_WCHUNK;
    s_written = off;
    if (s_stop) {
      close(s_wfd);
      free(buf);
      return NULL;
    }
  }
}
static void* stream_reader(void* arg) {
  (void)arg;
  unsigned char* buf = malloc(STREAM_RCHUNK);
  uint64_t off = 0;
  for (;;) {
    ssize_t n = read(s_rfd, buf, STREAM_RCHUNK);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) { ++s_eintr; continue; }
      ++s_errors;
      break;
    }
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] != stream_byte(off + i)) {
        ++s_rmismatch;
        break;
      }
    }
    off += (uint64_t)n;
    s_read = off;
  }
  s_stop = 1;
  free(buf);
  return NULL;
}
static int child_stream(int wfd, int rfd, int report) {
  signal(SIGPIPE, SIG_IGN);
  s_wfd = wfd;
  s_rfd = rfd;
  pthread_t w, r;
  pthread_create(&w, NULL, stream_writer, NULL);
  pthread_create(&r, NULL, stream_reader, NULL);
  if (write(report, "R", 1) != 1) return 2;
  pthread_join(r, NULL);
  pthread_join(w, NULL);
  char text[256];
  int len = snprintf(text, sizeof text, "%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                     (uint64_t)s_written, (uint64_t)s_read, (uint64_t)s_rmismatch,
                     (uint64_t)s_eintr, (uint64_t)s_errors);
  if (write(report, text, (size_t)len) != len) return 3;
  close(report);
  return 0;
}

static int st_drain_fd, st_feed_fd;
static volatile int st_stop_feed;
static volatile uint64_t st_drained, st_drain_mismatch, st_fed;
static void* stream_drainer(void* arg) {
  (void)arg;
  unsigned char buf[4096];
  uint64_t off = 0;
  for (;;) {
    ssize_t n = read(st_drain_fd, buf, sizeof buf);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] != stream_byte(off + i)) {
        ++st_drain_mismatch;
        break;
      }
    }
    off += (uint64_t)n;
    st_drained = off;
    if (!st_stop_feed) sleep_ms(rand() % 2);
  }
  return NULL;
}
static void* stream_feeder(void* arg) {
  (void)arg;
  unsigned char buf[3000];
  uint64_t off = 0;
  while (!st_stop_feed) {
    size_t len = 500 + (size_t)(rand() % 2500);
    for (size_t i = 0; i < len; ++i) buf[i] = stream_byte(off + i);
    size_t done = 0;
    while (done < len) {
      ssize_t n = write(st_feed_fd, buf + done, len - done);
      if (n < 0) {
        if (errno == EINTR) continue;
        return NULL;
      }
      done += (size_t)n;
    }
    off += len;
    st_fed = off;
    sleep_ms(rand() % 2);
  }
  return NULL;
}
static void parent_stream(int seize, int cycles) {
  const char* mode = seize ? "seize" : "attach";
  st_stop_feed = 0;
  st_drained = st_drain_mismatch = st_fed = 0;
  int pa[2], pb[2], rp[2];
  if (pipe2(pa, O_CLOEXEC) != 0 || pipe2(pb, O_CLOEXEC) != 0 || pipe2(rp, O_CLOEXEC) != 0) {
    check(0, "stream: pipes");
    return;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pa[1], 20);
  posix_spawn_file_actions_adddup2(&fa, pb[0], 21);
  posix_spawn_file_actions_adddup2(&fa, rp[1], 22);
  char* args[] = {"ptrace-contract", (char*)self_path, "stream", "20", "21", "22", NULL};
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, self_path, &fa, NULL, args, environ) == 0;
  posix_spawn_file_actions_destroy(&fa);
  close(pa[1]);
  close(pb[0]);
  close(rp[1]);
  char c = 0;
  if (!spawned || read(rp[0], &c, 1) != 1) {
    check(0, "stream: the tracee started");
    return;
  }
  st_drain_fd = pa[0];
  st_feed_fd = pb[1];
  pthread_t drainer, feeder;
  pthread_create(&drainer, NULL, stream_drainer, NULL);
  pthread_create(&feeder, NULL, stream_feeder, NULL);
  sleep_ms(100);
  int stops = 0;
  if (seize && ptrace(PTRACE_SEIZE, pid, 0, 0) != 0) cycles = 0;
  for (int i = 0; i < cycles; ++i) {
    if (seize ? ptrace(PTRACE_INTERRUPT, pid, 0, 0) != 0 : ptrace(PTRACE_ATTACH, pid, 0, 0) != 0) break;
    int status = 0, got = 0;
    int64_t t0 = now_ms();
    while (!got && now_ms() - t0 < 5000) {
      if (waitpid(pid, &status, WNOHANG | __WALL) == pid) got = WIFSTOPPED(status);
      else sleep_ms(1);
    }
    if (!got) break;
    ++stops;
    sleep_ms(rand() % 3); /* stopped while the tracer drains and feeds */
    if (seize ? ptrace(PTRACE_CONT, pid, 0, 0) != 0 : ptrace(PTRACE_DETACH, pid, 0, 0) != 0) break;
    sleep_ms(rand() % 4);
  }
  if (seize) ptrace(PTRACE_DETACH, pid, 0, 0);
  st_stop_feed = 1;
  pthread_join(feeder, NULL);
  sleep_ms(200);
  close(st_feed_fd); /* the tracee's reader sees EOF; its writer then ends */
  pthread_join(drainer, NULL);
  char text[256] = {0};
  size_t got = 0;
  for (;;) {
    ssize_t n = read(rp[0], text + got, sizeof text - 1 - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(rp[0]);
  close(st_drain_fd);
  wait_exit(pid);
  unsigned long long w = 0, r = 0, rm = 0, eintr = 0, errors = 0;
  sscanf(text + (text[0] == 'R' ? 1 : 0), "%llu %llu %llu %llu %llu", &w, &r, &rm, &eintr, &errors);
  printf("INFO  ptrace: stream %s: %d stops; tracee wrote %llu, tracer drained %llu (%llu mismatched"
         " reads); tracer fed %llu, tracee read %llu (%llu mismatched reads); EINTR %llu, errors %llu\n",
         mode, stops, w, (unsigned long long)st_drained, (unsigned long long)st_drain_mismatch,
         (unsigned long long)st_fed, r, rm, eintr, errors);
  char name[160];
  snprintf(name, sizeof name, "stream %s: %d stops of a tracee in large pipe writes and reads", mode, cycles);
  check(stops == cycles, name);
  snprintf(name, sizeof name, "stream %s: every byte written arrives once, in order", mode);
  check(st_drain_mismatch == 0 && st_drained == w && w > 0, name);
  snprintf(name, sizeof name, "stream %s: every byte fed is read once, in order", mode);
  check(rm == 0 && r == st_fed && r > 0, name);
  snprintf(name, sizeof name, "stream %s: no EINTR or error from a stop", mode);
  check(eintr == 0 && errors == 0, name);
}

/* --- a write description shared by the tracer and a stopped writer (qa8) ----
 * The tracee blocks in one 1 MB write() to a pipe whose write description the
 * tracer shares (inherited). While the tracee is stopped: it makes no more
 * progress than the pipe holds, and the tracer's poll(POLLOUT), write() and
 * F_SETFL on that shared description work at once, as on Linux. */
#define BIGWRITE 1000000
static int child_bigwriter(int wfd, int report) {
  unsigned char* buf = malloc(BIGWRITE);
  for (int i = 0; i < BIGWRITE; ++i) buf[i] = stream_byte((uint64_t)i);
  if (write(report, "R", 1) != 1) return 2;
  ssize_t n = write(wfd, buf, BIGWRITE);
  char text[32];
  int len = snprintf(text, sizeof text, "%zd\n", n);
  if (write(report, text, (size_t)len) != len) return 3;
  return n == BIGWRITE ? 0 : 4;
}
static volatile int sw_done;
static volatile ssize_t sw_result;
static int sw_fd;
static void* shared_writer(void* arg) {
  (void)arg;
  char x[16];
  memset(x, 'X', sizeof x);
  sw_result = write(sw_fd, x, sizeof x);
  sw_done = 1;
  return NULL;
}
static void parent_shared_write(void) {
  int pa[2], rp[2];
  if (pipe2(pa, O_CLOEXEC) != 0 || pipe2(rp, O_CLOEXEC) != 0) {
    check(0, "shared write: pipes");
    return;
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pa[1], 20);
  posix_spawn_file_actions_adddup2(&fa, rp[1], 22);
  char* args[] = {"ptrace-contract", (char*)self_path, "bigwriter", "20", "22", NULL};
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, self_path, &fa, NULL, args, environ) == 0;
  posix_spawn_file_actions_destroy(&fa);
  close(rp[1]);
  char c = 0;
  if (!spawned || read(rp[0], &c, 1) != 1) {
    check(0, "shared write: the writer started");
    return;
  }
  sw_fd = pa[1]; /* the same write description as the tracee's fd 20 */
  sw_done = 0;
  sleep_ms(300); /* its 1 MB write has filled the pipe and blocks */
  check(ptrace(PTRACE_ATTACH, pid, 0, 0) == 0, "shared write: ATTACH");
  int status = 0, stopped = 0;
  int64_t t0 = now_ms();
  while (!stopped && now_ms() - t0 < 5000) {
    if (waitpid(pid, &status, WNOHANG | __WALL) == pid) stopped = status == 0x137f;
    else sleep_ms(2);
  }
  check(stopped, "shared write: the writer blocked in a 1 MB write() stops");
  /* Drain what the stopped writer has in the pipe. */
  fcntl(pa[0], F_SETFL, O_NONBLOCK);
  unsigned char* buf = malloc(65536);
  uint64_t got = 0, mismatches = 0;
  t0 = now_ms();
  while (now_ms() - t0 < 1000) {
    ssize_t n = read(pa[0], buf, 65536);
    if (n > 0) {
      for (ssize_t i = 0; i < n; ++i) mismatches += buf[i] != stream_byte(got + (uint64_t)i);
      got += (uint64_t)n;
      continue;
    }
    sleep_ms(5);
  }
  const uint64_t while_stopped = got;
  printf("INFO  ptrace: shared write: %llu bytes read while the writer was stopped\n",
         (unsigned long long)while_stopped);
  check(while_stopped <= 65536 + 4096,
        "shared write: a stopped writer makes no progress beyond what the pipe holds");
  struct pollfd pf = {.fd = pa[1], .events = POLLOUT};
  const int polled = poll(&pf, 1, 0);
  check(polled == 1 && (pf.revents & POLLOUT) != 0,
        "shared write: the tracer's poll(POLLOUT) on the shared write description");
  /* While the pipe is empty: Gesher's F_SETFL on a pipe fails with EOPNOTSUPP
   * while the pipe holds data (Windows refuses the mode change then), stopped
   * writer or not. */
  const int flags = fcntl(pa[1], F_GETFL);
  const int set = fcntl(pa[1], F_SETFL, flags | O_NONBLOCK);
  check(set == 0, "shared write: F_SETFL on the shared description");
  if (set == 0) fcntl(pa[1], F_SETFL, flags);
  pthread_t w;
  pthread_create(&w, NULL, shared_writer, NULL);
  t0 = now_ms();
  while (!sw_done && now_ms() - t0 < 3000) sleep_ms(1);
  check(sw_done && sw_result == 16, "shared write: the tracer's write() on the shared description completes at once");
  check(ptrace(PTRACE_DETACH, pid, 0, 0) == 0, "shared write: DETACH");
  pthread_join(w, NULL);
  close(pa[1]);
  fcntl(pa[0], F_SETFL, 0);
  uint64_t xs = 0;
  for (;;) {
    ssize_t n = read(pa[0], buf, 65536);
    if (n <= 0) break;
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == 'X' && stream_byte(got) != 'X') {
        ++xs;
        continue;
      }
      mismatches += buf[i] != stream_byte(got);
      ++got;
    }
  }
  free(buf);
  char text[64] = {0};
  if (read(rp[0], text, sizeof text - 1) < 0) text[0] = 0;
  close(rp[0]);
  close(pa[0]);
  const int code = wait_exit(pid);
  check(code == 0 && atoll(text) == BIGWRITE,
        "shared write: the writer's write() returns all 1 MB after it is resumed");
  check(got == BIGWRITE && xs == 16 && mismatches == 0,
        "shared write: every byte arrives once, the tracer's 16 between whole pieces");
}

/* PTRACE_O_EXITKILL: the tracer's exit kills the tracee; without it the
 * tracee runs on. */
static void parent_exitkill(void) {
  for (int exitkill = 1; exitkill >= 0; --exitkill) {
    char line[64];
    char* args[] = {"ptrace-contract", (char*)self_path, exitkill ? "exitkill" : "noexitkill", NULL, NULL};
    pid_t tracer = spawn_reporting(args, line, sizeof line);
    pid_t victim = (pid_t)atoi(line);
    check(tracer > 0 && victim > 0 && wait_exit(tracer) == 0, "exitkill: the tracer seized its child and exited");
    if (victim <= 0) continue;
    int alive = 1;
    for (int i = 0; i < 300 && alive; ++i) {
      alive = kill(victim, 0) == 0;
      if (alive) sleep_ms(10);
    }
    if (exitkill) {
      check(!alive, "exitkill: PTRACE_O_EXITKILL kills the tracee when the tracer exits");
    } else {
      check(alive, "exitkill: without it the tracee runs on after the tracer exits");
      kill(victim, SIGKILL);
    }
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 2) {
    fprintf(stderr, "usage: ptrace-contract <self path> [role ...]\n");
    return 2;
  }
  self_path = argv[1];
  snprintf(work_dir, sizeof work_dir, "%s", self_path);
  char* slash = strrchr(work_dir, '/');
  if (slash != NULL) *slash = 0;
  else snprintf(work_dir, sizeof work_dir, ".");
  if (argc >= 6 && strcmp(argv[2], "peekpoke") == 0) {
    return child_peekpoke((uintptr_t)strtoull(argv[3], NULL, 16), (uintptr_t)strtoull(argv[4], NULL, 16),
                          strtoull(argv[5], NULL, 16));
  }
  if (argc >= 4 && strcmp(argv[2], "attach-eperm") == 0) return child_attach_eperm(atoi(argv[3]));
  if (argc >= 3 && strcmp(argv[2], "attach-exit") == 0) return child_attach_exit();
  if (argc >= 3 && strcmp(argv[2], "attach-detach") == 0) return child_attach_detach();
  if (argc >= 4 && strcmp(argv[2], "victim") == 0) return child_victim(atoi(argv[3]));
  if (argc >= 3 && strcmp(argv[2], "traceme") == 0) return child_traceme();
  if (argc >= 4 && strcmp(argv[2], "codevictim") == 0) return child_codevictim(atoi(argv[3]), 0);
  if (argc >= 4 && strcmp(argv[2], "spinvictim") == 0) return child_codevictim(atoi(argv[3]), 1);
  if (argc >= 4 && strcmp(argv[2], "pipetracer") == 0) return child_pipetracer(atoi(argv[3]));
  if (argc >= 3 && strcmp(argv[2], "sleeper") == 0) return child_sleeper();
  if (argc >= 5 && strcmp(argv[2], "bigwriter") == 0) return child_bigwriter(atoi(argv[3]), atoi(argv[4]));
  if (argc >= 6 && strcmp(argv[2], "stream") == 0) return child_stream(atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
  if (argc >= 5 && strcmp(argv[2], "sharedreader") == 0) return child_sharedreader(atoi(argv[4]), atoi(argv[3]));
  if (argc >= 5 && strcmp(argv[2], "lockvictim") == 0) return child_lockvictim(atoi(argv[3]), argv[4]);
  if (argc >= 4 && strcmp(argv[2], "lockone") == 0) return child_lockone(argv[3]);
  if (argc >= 4 && strcmp(argv[2], "exitkill") == 0) return child_exitkill_tracer(atoi(argv[3]), 1);
  if (argc >= 4 && strcmp(argv[2], "noexitkill") == 0) return child_exitkill_tracer(atoi(argv[3]), 0);

#if defined(__aarch64__)
  printf("ptrace contract ABI: arm64-v8a\n");
#else
  printf("ptrace contract ABI: x86_64\n");
#endif
  pid_t self = getpid();
  char self_text[16];
  snprintf(self_text, sizeof self_text, "%d", (int)self);

  /* Requests that need no other process. */
  errno = 0;
  check(ptrace(PTRACE_ATTACH, self, 0, 0) == -1 && errno == EPERM, "ATTACH to itself is EPERM");
  errno = 0;
  check(ptrace(PTRACE_ATTACH, 0x7ffffff0, 0, 0) == -1 && errno == ESRCH, "ATTACH to a missing pid is ESRCH");
  errno = 0;
  check(ptrace(PTRACE_PEEKDATA, getppid(), (void*)&self, 0) == -1 && errno == ESRCH,
        "PEEKDATA of a process it does not trace is ESRCH");
  check(prctl(PR_GET_DUMPABLE) == 1, "PR_GET_DUMPABLE is 1");
  errno = 0;
  check(prctl(PR_SET_DUMPABLE, 2) == -1 && errno == EINVAL, "PR_SET_DUMPABLE 2 is EINVAL");
  errno = 0;
  check(prctl(PR_SET_PTRACER, 0x7ffffff0UL) == -1 && errno == EINVAL,
        "PR_SET_PTRACER of a missing pid is EINVAL");
  check(tracer_pid() == 0, "TracerPid is 0 while untraced");

  /* Yama: a child is no ancestor of its parent. */
  {
    char* args[] = {"ptrace-contract", (char*)self_path, "attach-eperm", self_text, NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0, "a child's ATTACH without PR_SET_PTRACER is EPERM");
  }
  /* PR_SET_PTRACER naming a process allows it and its descendants only. */
  {
    uintptr_t value = 0, flag = 0;
    pid_t other = start_victim(&value, &flag);
    check(other > 0 && prctl(PR_SET_PTRACER, (unsigned long)other) == 0,
          "PR_SET_PTRACER of another process's pid");
    char* args[] = {"ptrace-contract", (char*)self_path, "attach-eperm", self_text, NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0, "a child not named by PR_SET_PTRACER is EPERM");
    if (other > 0) {
      kill(other, SIGKILL);
      wait_exit(other);
    }
  }
  check(prctl(PR_SET_PTRACER, (unsigned long)getppid()) == 0, "PR_SET_PTRACER of the parent's pid");
  {
    char* args[] = {"ptrace-contract", (char*)self_path, "attach-detach", NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0,
          "a descendant of the process PR_SET_PTRACER named may attach (and detach)");
  }
  check(prctl(PR_SET_PTRACER, 0UL) == 0, "PR_SET_PTRACER 0 clears the exception");
  {
    char* args[] = {"ptrace-contract", (char*)self_path, "attach-eperm", self_text, NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0, "after PR_SET_PTRACER 0 a child's ATTACH is EPERM again");
  }
  /* A process that is not dumpable cannot be attached to. */
  check(prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) == 0, "PR_SET_PTRACER_ANY");
  check(prctl(PR_SET_DUMPABLE, 0) == 0 && prctl(PR_GET_DUMPABLE) == 0, "PR_SET_DUMPABLE 0");
  {
    char* args[] = {"ptrace-contract", (char*)self_path, "attach-eperm", self_text, NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0, "ATTACH to a process that is not dumpable is EPERM");
  }
  check(prctl(PR_SET_DUMPABLE, 1) == 0 && prctl(PR_GET_DUMPABLE) == 1, "PR_SET_DUMPABLE 1");

  /* The PairIP sequence: the child attaches to its parent, which waits for it,
   * and copies one of the parent's stack slots into another. */
  {
    volatile uint64_t slots[2];
    slots[0] = 0x0123456789abcdefULL ^ (uint64_t)self;
    slots[1] = 0;
    char src[32], dst[32], value[32];
    snprintf(src, sizeof src, "%" PRIxPTR, (uintptr_t)&slots[0]);
    snprintf(dst, sizeof dst, "%" PRIxPTR, (uintptr_t)&slots[1]);
    snprintf(value, sizeof value, "%" PRIx64, (uint64_t)slots[0]);
    char* args[] = {"ptrace-contract", (char*)self_path, "peekpoke", src, dst, value, NULL};
    pid_t child = spawn(args);
    int status = -1;
    check(child > 0 && waitpid(child, &status, __WALL) == child, "the parent waits for the tracer child");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the tracer child passed its checks");
    check(slots[1] == slots[0], "the child copied the parent's slot through PEEKDATA/POKEDATA");
    check(tracer_pid() == 0, "TracerPid is 0 after the child detached");
  }

  /* A tracer that exits without detaching releases its tracee. */
  {
    char* args[] = {"ptrace-contract", (char*)self_path, "attach-exit", NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0, "a tracer child exits without detaching; the parent runs on");
    check(tracer_pid() == 0, "TracerPid is 0 after the tracer exited");
  }

  /* PTRACE_TRACEME: once, then EPERM, and TracerPid names the parent. */
  {
    char* args[] = {"ptrace-contract", (char*)self_path, "traceme", NULL};
    pid_t child = spawn(args);
    check(child > 0 && wait_exit(child) == 0, "TRACEME succeeds once, then EPERM; TracerPid is the parent");
  }

  parent_attach_detach();
  parent_seize_interrupt();
  parent_uid();
  parent_poke_code("codevictim", PTRACE_POKETEXT);
  parent_poke_code("codevictim", PTRACE_POKEDATA);
  parent_poke_code("spinvictim", PTRACE_POKETEXT);
  parent_pipe();
  parent_shared_pipe();
  parent_shared_write();
  parent_stream(0, 60);
  parent_stream(1, 60);
  parent_attach_at_spawn();
  parent_exitkill();
  parent_locks(20);

  printf("ptrace contract: %d failed\n", failed);
  return failed == 0 ? 0 : 1;
}
