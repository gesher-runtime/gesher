// gesher-base: <sys/syscall.h>. syscall() emulates the handful of Linux
// system calls AOSP code invokes directly (notably futex and gettid); the
// rest fail with ENOSYS. Numbers are the Linux x86-64 ones.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define __NR_read            0
#define __NR_write           1
#define __NR_mmap            9
#define __NR_mprotect        10
#define __NR_madvise         28
#define __NR_getpid          39
#define __NR_clone           56
#define __NR_kill            62
#define __NR_prctl           157
#define __NR_arch_prctl      158
#define __NR_gettid          186
#define __NR_tkill           200
#define __NR_futex           202
#define __NR_sched_setaffinity 203
#define __NR_sched_getaffinity 204
#define __NR_tgkill          234
#define __NR_membarrier      324
#define __NR_userfaultfd     323
#define __NR_memfd_create    319
#define __NR_pidfd_open      434
#define __NR_process_madvise 440

#define SYS_read            __NR_read
#define SYS_write           __NR_write
#define SYS_mmap            __NR_mmap
#define SYS_mprotect        __NR_mprotect
#define SYS_madvise         __NR_madvise
#define SYS_getpid          __NR_getpid
#define SYS_clone           __NR_clone
#define SYS_kill            __NR_kill
#define SYS_prctl           __NR_prctl
#define SYS_arch_prctl      __NR_arch_prctl
#define SYS_gettid          __NR_gettid
#define SYS_tkill           __NR_tkill
#define SYS_futex           __NR_futex
#define SYS_sched_setaffinity __NR_sched_setaffinity
#define SYS_sched_getaffinity __NR_sched_getaffinity
#define SYS_tgkill          __NR_tgkill
#define SYS_membarrier      __NR_membarrier
#define SYS_userfaultfd     __NR_userfaultfd
#define SYS_memfd_create    __NR_memfd_create
#define SYS_pidfd_open      __NR_pidfd_open
#define SYS_process_madvise __NR_process_madvise

long syscall(long number, ...);

#ifdef __cplusplus
}
#endif
