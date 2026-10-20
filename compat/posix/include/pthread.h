// gesher-base: Linux additions to winpthreads' <pthread.h>.
#pragma once

#include_next <pthread.h>

// winpthreads includes its own "sched.h" by quoted path, which skips
// gesher-base's; Linux code expects CLONE_* and friends from here.
#include <sched.h>

// winpthreads defines these as macros that expand to ENOTSUP.
#undef pthread_getcpuclockid
#undef pthread_attr_getguardsize
#undef pthread_attr_setguardsize

#ifdef __cplusplus
extern "C" {
#endif

// Describes a running thread's stack. Only the calling thread is supported.
int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);

// Windows has no per-thread CPU clock id; the returned clock reads the
// calling thread's CPU time, so it is only exact for pthread_self().
int pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id);

// The guard size of Windows thread stacks is one page; setting it is
// accepted and ignored.
int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize);
int pthread_attr_setguardsize(pthread_attr_t* attr, size_t guardsize);

#ifdef __cplusplus
}
#endif
