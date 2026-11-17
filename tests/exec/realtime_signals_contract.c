// SPDX-License-Identifier: Apache-2.0
// One bounded public-API contract for Linux, actual Windows POSIX, and NDK ELF.
// No production behavior is replaced. Process creation is tested by NDK/Linux.
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#if !defined(_WIN32)
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

static int failures;
static void check(int okay, const char* label) {
  printf("%s %s\n", okay ? "PASS" : "FAIL", label);
  if (!okay) ++failures;
}
static sigset_t one(int sig) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
  return set;
}
static int take(int sig, siginfo_t* info, int seconds) {
  sigset_t set = one(sig);
  const struct timespec timeout = {seconds, 0};
  return sigtimedwait(&set, info, &timeout);
}
static void step(void) {
  const struct timespec delay = {0, 10000000};
  nanosleep(&delay, NULL);
}
static void drain(int sig) {
  siginfo_t info;
  for (int i = 0; i < 128 && take(sig, &info, 0) == sig; ++i) {}
}

static void fifo(void) {
  int sig = SIGRTMIN;
  sigset_t set = one(sig), old;
  check(pthread_sigmask(SIG_BLOCK, &set, &old) == 0, "block RT signal");
  int sent = 0;
  for (; sent < 40; ++sent) {
    union sigval value = {.sival_int = 100 + sent};
    if (sigqueue(getpid(), sig, value) != 0) break;
  }
  check(sent == 40, "forty RT sends exceed former arbitrary 32 cap");
  int ordered = 1, identity = 1;
  for (int i = 0; i < sent; ++i) {
    siginfo_t info = {0};
    ordered &= take(sig, &info, 0) == sig && info.si_value.sival_int == 100 + i;
    identity &= info.si_code == SI_QUEUE && info.si_pid == getpid() && info.si_uid == getuid();
  }
  check(ordered, "RT records retain FIFO values");
  check(identity, "RT records carry actual sender pid and real uid");
  siginfo_t info;
  check(take(sig, &info, 0) == -1 && errno == EAGAIN, "all RT records consumed exactly once");
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void ignored(void) {
  int sig = SIGRTMIN;
  struct sigaction action = {0}, oldaction;
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  sigset_t set = one(sig), old;
  pthread_sigmask(SIG_UNBLOCK, &set, &old);
  check(sigaction(sig, &action, &oldaction) == 0, "install ignore disposition");
  int sent = 0;
  for (; sent < 40; ++sent) {
    union sigval value = {.sival_int = sent};
    if (sigqueue(getpid(), sig, value) != 0) break;
  }
  check(sent == 40, "unblocked ignored signals do not consume queue resources");
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  union sigval value = {.sival_int = 71};
  check(sigqueue(getpid(), sig, value) == 0, "blocked ignored signal can be queued");
  siginfo_t info = {0};
  check(take(sig, &info, 0) == sig && info.si_value.sival_int == 71,
        "blocked ignored signal remains synchronously waitable");
  for (int i = 0; i < 4; ++i) sigqueue(getpid(), sig, value);
  check(sigaction(sig, &action, NULL) == 0, "reset ignore disposition");
  check(take(sig, &info, 0) == -1 && errno == EAGAIN,
        "SIG_IGN flushes every queued RT instance despite blocked mask");
  sigaction(sig, &oldaction, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void quota(void) {
  int sig = SIGRTMIN;
  sigset_t set = one(sig), old;
  pthread_sigmask(SIG_BLOCK, &set, &old);
  struct rlimit limit;
  int got = getrlimit(RLIMIT_SIGPENDING, &limit);
  check(got == 0, "read actual pending-signal resource limit");
  if (got != 0) return;
  struct rlimit zero = limit;
  zero.rlim_cur = 0;
  int changed = setrlimit(RLIMIT_SIGPENDING, &zero);
  check(changed == 0, "lower only this process's soft queue limit to zero");
  if (changed == 0) {
    union sigval value = {.sival_int = 91};
    errno = 0;
    check(sigqueue(getpid(), sig, value) == -1 && errno == EAGAIN,
          "RT sigqueue reports EAGAIN at configured quota");
    check(kill(getpid(), sig) == 0, "RT kill survives queue quota exhaustion");
    check(setrlimit(RLIMIT_SIGPENDING, &limit) == 0, "restore original resource limit");
    siginfo_t info = {0};
    check(take(sig, &info, 0) == sig, "RT kill fallback remains waitable");
    check(info.si_code == SI_USER && info.si_pid == 0 && info.si_uid == 0 &&
              info.si_value.sival_ptr == NULL, "allocation fallback does not invent siginfo");
  }
  drain(sig);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void priority(void) {
  sigset_t set, old;
  sigemptyset(&set);
  for (int i = 0; i < 3; ++i) sigaddset(&set, SIGRTMIN + i);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  for (int i = 2; i >= 0; --i) {
    for (int v = 1; v <= 2; ++v) {
      union sigval value = {.sival_int = i * 10 + v};
      check(sigqueue(getpid(), SIGRTMIN + i, value) == 0, "enqueue priority fixture");
    }
  }
  const struct timespec zero = {0, 0};
  int ordered = 1;
  for (int i = 0; i < 3; ++i) {
    for (int v = 1; v <= 2; ++v) {
      siginfo_t info = {0};
      ordered &= sigtimedwait(&set, &info, &zero) == SIGRTMIN + i && info.si_value.sival_int == i * 10 + v;
    }
  }
  check(ordered, "lowest RT signal number first and FIFO within each number");
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void timeout_errors(void) {
  sigset_t set = one(SIGRTMIN), old;
  pthread_sigmask(SIG_BLOCK, &set, &old);
  const struct timespec invalid[] = {{-1, 0}, {0, -1}, {0, 1000000000}};
  for (int i = 0; i < 3; ++i) {
    errno = 0;
    check(sigtimedwait(&set, NULL, &invalid[i]) == -1 && errno == EINVAL,
          "invalid timeout returns EINVAL");
  }
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static atomic_int thread_ready, thread_go, thread_ok, inherited;
static void* targeted(void* unused) {
  (void)unused;
  sigset_t old, set = one(SIGRTMIN);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  atomic_store(&inherited, sigismember(&old, SIGUSR2) == 1);
  atomic_store(&thread_ready, 1);
  for (int i = 0; i < 300 && !atomic_load(&thread_go); ++i) step();
  int okay = 1;
  for (int i = 0; i < 3; ++i) {
    siginfo_t info = {0};
    // glibc's sigtimedwait wrapper rewrites SI_TKILL to SI_USER. Bionic's
    // pinned wrapper forwards the kernel result unchanged; neither Gesher
    // role gets the glibc-only normalization in this Linux reference build.
#if defined(__GLIBC__)
    const int expected_code = SI_USER;
#else
    const int expected_code = SI_TKILL;
#endif
    okay &= take(SIGRTMIN, &info, 1) == SIGRTMIN && info.si_code == expected_code &&
            info.si_pid == getpid() && info.si_uid == getuid();
  }
  atomic_store(&thread_ok, okay);
  return NULL;
}
static void thread_target(void) {
  sigset_t set = one(SIGRTMIN), old;
  sigaddset(&set, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  pthread_t thread;
  int created = pthread_create(&thread, NULL, targeted, NULL);
  check(created == 0, "create ordinary target pthread");
  if (created != 0) return;
  for (int i = 0; i < 300 && !atomic_load(&thread_ready); ++i) step();
  check(atomic_load(&thread_ready), "target publishes its blocked mask");
  for (int i = 0; i < 3; ++i) check(pthread_kill(thread, SIGRTMIN) == 0, "send RT signal to target pthread");
  siginfo_t info;
  check(take(SIGRTMIN, &info, 0) == -1 && errno == EAGAIN,
        "another thread cannot consume target's pending signal");
  atomic_store(&thread_go, 1);
  check(pthread_join(thread, NULL) == 0, "join actual target pthread");
  check(atomic_load(&thread_ok), "target consumes all three RT instances with sender identity");
  check(atomic_load(&inherited), "pthread inherits its creator's blocked mask");
  drain(SIGRTMIN);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static unsigned char alternate[65536];
static volatile sig_atomic_t handler_stack, handler_mask, context_mask, context_pc, handler_value;
static void inspect_handler(int sig, siginfo_t* info, void* raw) {
  unsigned char local;
  uintptr_t at = (uintptr_t)&local;
  handler_stack = at >= (uintptr_t)alternate && at < (uintptr_t)alternate + sizeof(alternate);
  sigset_t mask;
  pthread_sigmask(SIG_BLOCK, NULL, &mask);
  handler_mask = sigismember(&mask, SIGUSR2) == 1 && sigismember(&mask, sig) == 1;
  ucontext_t* context = (ucontext_t*)raw;
  context_mask = sigismember(&context->uc_sigmask, SIGUSR1) == 1 &&
                 sigismember(&context->uc_sigmask, SIGUSR2) == 1;
#if defined(__aarch64__)
  context_pc = context->uc_mcontext.pc != 0;
#elif defined(__x86_64__)
  context_pc = context->uc_mcontext.gregs[REG_RIP] != 0;
#endif
  handler_value = info->si_value.sival_int;
}
static void handler(void) {
  sigset_t set = one(SIGUSR1), old;
  sigaddset(&set, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  stack_t stack = {0}, oldstack;
  stack.ss_sp = alternate;
  stack.ss_size = sizeof(alternate);
  check(sigaltstack(&stack, &oldstack) == 0, "register alternate stack");
  struct sigaction action = {0}, oldaction;
  action.sa_sigaction = inspect_handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  check(sigaction(SIGUSR1, &action, &oldaction) == 0, "install ordinary SA_SIGINFO SA_ONSTACK handler");
  union sigval value = {.sival_int = 456};
  check(sigqueue(getpid(), SIGUSR1, value) == 0, "queue signal for sigsuspend");
  sigset_t temporary;
  sigfillset(&temporary);
  sigdelset(&temporary, SIGUSR1);
  check(sigsuspend(&temporary) == -1 && errno == EINTR, "sigsuspend returns EINTR after handler");
  check(handler_stack, "handler actually runs on registered alternate stack");
  check(handler_mask, "handler runs with temporary mask and deferred signal blocked");
  check(context_mask, "ucontext contains pre-sigsuspend signal mask");
  check(context_pc, "ucontext contains actual interrupted program counter");
  check(handler_value == 456, "handler sees queued value");
  sigset_t after;
  pthread_sigmask(SIG_BLOCK, NULL, &after);
  check(sigismember(&after, SIGUSR1) == 1 && sigismember(&after, SIGUSR2) == 1,
        "sigsuspend restores original mask");
  sigaction(SIGUSR1, &oldaction, NULL);
  sigaltstack(&oldstack, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

#if !defined(_WIN32)
// All children are created by this fixture. No signal targets are discovered.
static int collect(pid_t pid, int* status, int options) {
  for (int i = 0; i < 200; ++i) {
    int result = waitpid(pid, status, options | WNOHANG);
    if (result != 0) return result;
    step();
  }
  return 0;
}
static pid_t spawn_child(const char* path, const char* mode, int* ready) {
  int channel[2];
  if (pipe(channel) != 0) return -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, channel[0]);
  posix_spawn_file_actions_adddup2(&actions, channel[1], 3);
  if (channel[1] != 3) posix_spawn_file_actions_addclose(&actions, channel[1]);
  char* args[] = {(char*)path, (char*)mode, NULL};
  pid_t pid = -1;
  int result = posix_spawn(&pid, path, &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(channel[1]);
  if (result != 0) { close(channel[0]); return -1; }
  *ready = channel[0];
  return pid;
}
static void cleanup_child(pid_t pid) {
  if (pid > 0) { kill(pid, SIGKILL); int status; collect(pid, &status, 0); }
}
static int child_cross(void) {
  sigset_t set = one(SIGRTMIN);
  sigaddset(&set, SIGRTMIN + 1);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  if (write(3, "R", 1) != 1) return 4;
  close(3);
  const struct timespec timeout = {2, 0};
  // Sender sends in this order, preserving repeated same-number RT records.
  for (int i = 0; i < 4; ++i) {
    siginfo_t info = {0};
    int sig = sigtimedwait(&set, &info, &timeout);
    check(sig == SIGRTMIN && info.si_code == SI_QUEUE && info.si_value.sival_int == 900 + i &&
              info.si_pid == getppid() && info.si_uid == getuid(),
          "cross-process RT value order and sender identity survive transport");
  }
  return failures ? 1 : 0;
}
static void cross_process(const char* path) {
  int ready = -1;
  pid_t pid = spawn_child(path, "--child-cross", &ready);
  check(pid > 0, "spawn owned ELF signal receiver");
  if (pid <= 0) return;
  // Child has a bounded wait; the CI supervisor also bounds the parent tree.
  char byte = 0;
  int initialized = read(ready, &byte, 1) == 1 && byte == 'R';
  close(ready);
  check(initialized, "owned child announces ready after blocking RT signal");
  if (!initialized) { cleanup_child(pid); return; }
  for (int i = 0; i < 4; ++i) {
    union sigval value = {.sival_int = 900 + i};
    check(sigqueue(pid, SIGRTMIN, value) == 0, "sigqueue to owned child succeeds");
  }
  int status = 0;
  int result = collect(pid, &status, 0);
  check(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "real child verifies all cross-process RT records");
  if (result == 0) cleanup_child(pid);
}
static void stop_continue(const char* path) {
  int ready = -1;
  pid_t pid = spawn_child(path, "--child-stop", &ready);
  check(pid > 0, "spawn owned stop/continue child");
  if (pid <= 0) return;
  close(ready);
  int status = 0;
  int stopped = collect(pid, &status, WUNTRACED);
  check(stopped == pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
        "SIGSTOP creates real stopped wait status without exiting");
  if (stopped == pid && WIFSTOPPED(status)) {
    check(kill(pid, SIGCONT) == 0, "SIGCONT resumes owned stopped process");
    int continued = collect(pid, &status, WCONTINUED);
    check(continued == pid && WIFCONTINUED(status), "waitpid reports continued state");
    int exited = collect(pid, &status, 0);
    check(exited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "resumed child exits normally");
    if (exited == 0) cleanup_child(pid);
  } else if (stopped == 0) cleanup_child(pid);
}
static void exit_status(const char* path) {
  const char* modes[] = {"--child-exit143", "--child-term"};
  for (int i = 0; i < 2; ++i) {
    int ready = -1;
    pid_t pid = spawn_child(path, modes[i], &ready);
    check(pid > 0, "spawn owned status child");
    if (pid <= 0) continue;
    close(ready);
    int status = 0;
    int result = collect(pid, &status, 0);
    check(result == pid && (i == 0 ? WIFEXITED(status) && WEXITSTATUS(status) == 143
                                  : WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM),
          i == 0 ? "normal exit143 is distinct from SIGTERM termination"
                 : "signal termination yields WIFSIGNALED and actual signal");
    if (result == 0) cleanup_child(pid);
  }
}
#endif

int main(int argc, char** argv) {
  if (argc < 2) return 2;
#if !defined(_WIN32)
  if (strcmp(argv[1], "--child-cross") == 0) return child_cross();
  if (strcmp(argv[1], "--child-stop") == 0) {
    close(3);
    raise(SIGSTOP);
    // Leave time for WCONTINUED to be collected before this child's exit.
    for (int i = 0; i < 80; ++i) step();
    return 0;
  }
  if (strcmp(argv[1], "--child-exit143") == 0) return 143;
  if (strcmp(argv[1], "--child-term") == 0) { kill(getpid(), SIGTERM); return 5; }
#endif
  printf("realtime contract: %s\n", argv[1]);
  if (strcmp(argv[1], "fifo") == 0) fifo();
  else if (strcmp(argv[1], "ignore") == 0) ignored();
  else if (strcmp(argv[1], "quota") == 0) quota();
  else if (strcmp(argv[1], "priority") == 0) priority();
  else if (strcmp(argv[1], "timeout") == 0) timeout_errors();
  else if (strcmp(argv[1], "thread") == 0) thread_target();
  else if (strcmp(argv[1], "handler") == 0) handler();
#if !defined(_WIN32)
  else if (argc == 3 && strcmp(argv[1], "cross") == 0) cross_process(argv[2]);
  else if (argc == 3 && strcmp(argv[1], "stop") == 0) stop_continue(argv[2]);
  else if (argc == 3 && strcmp(argv[1], "status") == 0) exit_status(argv[2]);
#endif
  else return 2;
  printf("realtime contract: %d failures\n", failures);
  return failures ? 1 : 0;
}
