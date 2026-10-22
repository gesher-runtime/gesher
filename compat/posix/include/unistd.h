// gesher-base: POSIX additions to mingw's <unistd.h>.
#pragma once

#include_next <unistd.h>

#include <errno.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// sysconf names, with glibc's values. libcore reads them through
// OsConstantsHolder, so they only need to match sysconf (src/unistd.cc).
#define _SC_ARG_MAX                      0
#define _SC_CHILD_MAX                    1
#define _SC_CLK_TCK                      2
#define _SC_NGROUPS_MAX                  3
#define _SC_OPEN_MAX                     4
#define _SC_STREAM_MAX                   5
#define _SC_TZNAME_MAX                   6
#define _SC_JOB_CONTROL                  7
#define _SC_SAVED_IDS                    8
#define _SC_REALTIME_SIGNALS             9
#define _SC_PRIORITY_SCHEDULING          10
#define _SC_TIMERS                       11
#define _SC_ASYNCHRONOUS_IO              12
#define _SC_PRIORITIZED_IO               13
#define _SC_SYNCHRONIZED_IO              14
#define _SC_FSYNC                        15
#define _SC_MAPPED_FILES                 16
#define _SC_MEMLOCK                      17
#define _SC_MEMLOCK_RANGE                18
#define _SC_MEMORY_PROTECTION            19
#define _SC_MESSAGE_PASSING              20
#define _SC_SEMAPHORES                   21
#define _SC_SHARED_MEMORY_OBJECTS        22
#define _SC_AIO_LISTIO_MAX               23
#define _SC_AIO_MAX                      24
#define _SC_AIO_PRIO_DELTA_MAX           25
#define _SC_DELAYTIMER_MAX               26
#define _SC_MQ_OPEN_MAX                  27
#define _SC_MQ_PRIO_MAX                  28
#define _SC_VERSION                      29
#define _SC_PAGESIZE                     30
#define _SC_PAGE_SIZE                    _SC_PAGESIZE
#define _SC_RTSIG_MAX                    31
#define _SC_SEM_NSEMS_MAX                32
#define _SC_SEM_VALUE_MAX                33
#define _SC_SIGQUEUE_MAX                 34
#define _SC_TIMER_MAX                    35
#define _SC_BC_BASE_MAX                  36
#define _SC_BC_DIM_MAX                   37
#define _SC_BC_SCALE_MAX                 38
#define _SC_BC_STRING_MAX                39
#define _SC_COLL_WEIGHTS_MAX             40
#define _SC_EXPR_NEST_MAX                42
#define _SC_LINE_MAX                     43
#define _SC_RE_DUP_MAX                   44
#define _SC_2_VERSION                    46
#define _SC_2_C_BIND                     47
#define _SC_2_C_DEV                      48
#define _SC_2_FORT_DEV                   49
#define _SC_2_FORT_RUN                   50
#define _SC_2_SW_DEV                     51
#define _SC_2_LOCALEDEF                  52
#define _SC_UIO_MAXIOV                   60
#define _SC_IOV_MAX                      _SC_UIO_MAXIOV
#define _SC_THREADS                      67
#define _SC_THREAD_SAFE_FUNCTIONS        68
#define _SC_GETGR_R_SIZE_MAX             69
#define _SC_GETPW_R_SIZE_MAX             70
#define _SC_LOGIN_NAME_MAX               71
#define _SC_TTY_NAME_MAX                 72
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 73
#define _SC_THREAD_KEYS_MAX              74
#define _SC_THREAD_STACK_MIN             75
#define _SC_THREAD_THREADS_MAX           76
#define _SC_THREAD_ATTR_STACKADDR        77
#define _SC_THREAD_ATTR_STACKSIZE        78
#define _SC_THREAD_PRIORITY_SCHEDULING   79
#define _SC_THREAD_PRIO_INHERIT          80
#define _SC_THREAD_PRIO_PROTECT          81
#define _SC_THREAD_PROCESS_SHARED        82
#define _SC_NPROCESSORS_CONF             83
#define _SC_NPROCESSORS_ONLN             84
#define _SC_PHYS_PAGES                   85
#define _SC_AVPHYS_PAGES                 86
#define _SC_ATEXIT_MAX                   87
#define _SC_PASS_MAX                     88
#define _SC_XOPEN_VERSION                89
#define _SC_XOPEN_XCU_VERSION            90
#define _SC_XOPEN_UNIX                   91
#define _SC_XOPEN_CRYPT                  92
#define _SC_XOPEN_ENH_I18N               93
#define _SC_XOPEN_SHM                    94
#define _SC_2_CHAR_TERM                  95
#define _SC_2_C_VERSION                  96
#define _SC_2_UPE                        97
#define _SC_XBS5_ILP32_OFF32             125
#define _SC_XBS5_ILP32_OFFBIG            126
#define _SC_XBS5_LP64_OFF64              127
#define _SC_XBS5_LPBIG_OFFBIG            128
#define _SC_XOPEN_LEGACY                 129
#define _SC_XOPEN_REALTIME               130
#define _SC_XOPEN_REALTIME_THREADS       131
#define _SC_HOST_NAME_MAX                180
#define _SC_MONOTONIC_CLOCK              149

// pathconf names, with glibc's values.
#define _PC_LINK_MAX           0
#define _PC_MAX_CANON          1
#define _PC_MAX_INPUT          2
#define _PC_NAME_MAX           3
#define _PC_PATH_MAX           4
#define _PC_PIPE_BUF           5
#define _PC_CHOWN_RESTRICTED   6
#define _PC_NO_TRUNC           7
#define _PC_VDISABLE           8
#define _PC_SYNC_IO            9
#define _PC_ASYNC_IO           10
#define _PC_PRIO_IO            11
#define _PC_FILESIZEBITS       13
#define _PC_REC_INCR_XFER_SIZE 14
#define _PC_REC_MAX_XFER_SIZE  15
#define _PC_REC_MIN_XFER_SIZE  16
#define _PC_REC_XFER_ALIGN     17
#define _PC_ALLOC_SIZE_MIN     18
#define _PC_SYMLINK_MAX        19
#define _PC_2_SYMLINKS         20

long sysconf(int name);
long pathconf(const char* path, int name);
long fpathconf(int fd, int name);
int getpagesize(void);

// Android apps run as a single uid; Gesher reports a fixed app uid. Owners
// cannot change: chown succeeds only when it changes nothing (-1 or the
// app's ids), and set*id only for the app's own ids.
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int seteuid(uid_t uid);
int setreuid(uid_t ruid, uid_t euid);
int setgid(gid_t gid);
int setegid(gid_t gid);
int setregid(gid_t rgid, gid_t egid);
int chown(const char* path, uid_t owner, gid_t group);
int lchown(const char* path, uid_t owner, gid_t group);
pid_t getppid(void);
pid_t gettid(void);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);

// Android apps cannot fork; neither can Gesher. Fails with ENOSYS.
// (execv and friends come from the Windows CRT.)
pid_t fork(void);

int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int fsync(int fd);
int fdatasync(int fd);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset);
ssize_t pread64(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite64(int fd, const void* buf, size_t count, off_t offset);
ssize_t readlink(const char* path, char* buf, size_t size);
int symlink(const char* target, const char* linkpath);
int link(const char* oldpath, const char* newpath);
int nice(int inc);

// gethostname under another name: <winsock2.h>, which some AOSP headers
// include on Windows (android-base/endian.h), declares gethostname with an
// int length, and ws2_32's symbol of that name would collide with one here.
// The host's DNS name in UTF-8; ENAMETOOLONG if it does not fit.
int gesher_gethostname(char* name, size_t len);

// The CRT's rmdir reports ENOTEMPTY with its own number (see <errno.h>).
int gesher_rmdir(const char* path);
#define rmdir gesher_rmdir

// glibc's, for calls that can fail with EINTR.
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    __typeof__(exp) _rc;                   \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif

#ifdef __cplusplus
}
#endif
