// gesher-base: Linux signal API on Windows, on top of mingw's <signal.h>.
//
// Synchronous signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP) are
// delivered from Windows exceptions by compat/posix/src/signal.cc, with a
// Linux-layout siginfo_t and ucontext_t (see <sys/ucontext.h>).
#pragma once

#include_next <signal.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

// winpthreads defines pthread_sigmask as a macro returning 0.
#undef pthread_sigmask

#ifdef __cplusplus
extern "C" {
#endif

// Signal numbers mingw lacks, with their Linux values. mingw keeps its own
// SIGABRT (22) and SIGBREAK (21).
#define SIGHUP    1
#define SIGQUIT   3
#define SIGTRAP   5
#define SIGBUS    7
#define SIGKILL   9
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31

#undef NSIG
#define NSIG  65
#define _NSIG 65
#define SIGRTMIN 34
#define SIGRTMAX 64

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// si_code values.
#define SI_USER    0
#define SI_KERNEL  0x80
#define SI_QUEUE   (-1)
#define SI_TKILL   (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define BUS_ADRALN  1
#define BUS_ADRERR  2
#define BUS_OBJERR  3
#define ILL_ILLOPC  1
#define ILL_ILLOPN  2
#define ILL_ILLADR  3
#define ILL_ILLTRP  4
#define ILL_PRVOPC  5
#define ILL_PRVREG  6
#define ILL_COPROC  7
#define ILL_BADSTK  8
#define FPE_INTDIV  1
#define FPE_INTOVF  2
#define FPE_FLTDIV  3
#define FPE_FLTOVF  4
#define FPE_FLTUND  5
#define FPE_FLTRES  6
#define FPE_FLTINV  7
#define FPE_FLTSUB  8
#define TRAP_BRKPT  1
#define TRAP_TRACE  2
#define SYS_SECCOMP 1
#define SI_TIMER   (-2)
#define SI_MESGQ   (-3)
#define SI_ASYNCIO (-4)
#define SI_SIGIO   (-5)

union sigval {
  int sival_int;
  void* sival_ptr;
};

// Linux x86-64 layout (128 bytes).
typedef struct {
  int si_signo;
  int si_errno;
  int si_code;
  int __pad0;
  union {
    int _pad[28];
    struct {
      pid_t si_pid;
      uid_t si_uid;
      union sigval si_value;
    } _kill;
    struct {
      pid_t si_pid;
      uid_t si_uid;
      int si_status;
      long long si_utime;
      long long si_stime;
    } _sigchld;
    struct {
      void* si_addr;
    } _sigfault;
    struct {
      void* si_call_addr;
      int si_syscall;
      unsigned int si_arch;
    } _sigsys;
  } _sifields;
} siginfo_t;

#define si_pid       _sifields._kill.si_pid
#define si_uid       _sifields._kill.si_uid
#define si_value     _sifields._kill.si_value
#define si_status    _sifields._sigchld.si_status
#define si_utime     _sifields._sigchld.si_utime
#define si_stime     _sifields._sigchld.si_stime
#define si_addr      _sifields._sigfault.si_addr
#define si_call_addr _sifields._sigsys.si_call_addr
#define si_syscall   _sifields._sigsys.si_syscall
#define si_arch      _sifields._sigsys.si_arch

// SIGCHLD si_code values.
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

typedef void (*sighandler_t)(int);

struct sigaction {
  union {
    sighandler_t sa_handler;
    void (*sa_sigaction)(int, siginfo_t*, void*);
  };
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};

typedef struct {
  void* ss_sp;
  int ss_flags;
  size_t ss_size;
} stack_t;

#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int signum);
int sigdelset(sigset_t* set, int signum);
int sigismember(const sigset_t* set, int signum);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset);
int sigaltstack(const stack_t* ss, stack_t* old_ss);
int sigwait(const sigset_t* set, int* sig);
int sigsuspend(const sigset_t* mask);
int sigpending(sigset_t* set);
int kill(pid_t pid, int sig);
int tgkill(pid_t tgid, pid_t tid, int sig);
int sigqueue(pid_t pid, int sig, const union sigval value);
int sigwaitinfo(const sigset_t* set, siginfo_t* info);
int sigtimedwait(const sigset_t* set, siginfo_t* info, const struct timespec* timeout);

// bionic's 64-bit-signal-set API; sigset_t already holds all 64 signals.
typedef sigset_t sigset64_t;
#define sigemptyset64 sigemptyset
#define sigfillset64 sigfillset
#define sigaddset64 sigaddset
#define sigdelset64 sigdelset
#define sigismember64 sigismember
#define sigprocmask64 sigprocmask
#define pthread_sigmask64 pthread_sigmask
#define sigwait64 sigwait
#define sigwaitinfo64 sigwaitinfo
#define sigtimedwait64 sigtimedwait

#ifdef __cplusplus
}
#endif

// Like bionic, <signal.h> provides ucontext_t.
#include <sys/ucontext.h>
