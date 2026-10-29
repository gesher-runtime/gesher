// gesher-base: Linux additions to winpthreads' <pthread.h>.
#pragma once

#include_next <pthread.h>

// winpthreads includes its own "sched.h" by quoted path, which skips
// gesher-base's; Linux code expects CLONE_* and friends from here.
#include <sched.h>
#include <errno.h>

// winpthreads defines these as macros that expand to ENOTSUP (or, for
// pthread_atfork, to 0, which hides the call).
#undef pthread_getcpuclockid
#undef pthread_attr_getguardsize
#undef pthread_attr_setguardsize
#undef pthread_atfork

#ifdef __cplusplus
extern "C" {
#endif

// Describes a running thread's stack. Only the calling thread is supported.
int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr);

// A target clock retains the exact kernel object authenticated by the supplier.
int pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id);

// The guard size of Windows thread stacks is one page; setting it is
// accepted and ignored.
int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize);
int pthread_attr_setguardsize(pthread_attr_t* attr, size_t guardsize);

// There is no fork (it fails with ENOSYS), so fork handlers never run;
// registering them succeeds, as libbinder's ProcessState requires.
int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void));

// winpthreads runs a key's destructor at thread exit whenever the key was
// set, even back to NULL; POSIX runs it only for non-NULL values (ART's
// thread-exit check relies on that). These wrap the destructor accordingly.
int gesher_pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int gesher_pthread_key_delete(pthread_key_t key);
#define pthread_key_create gesher_pthread_key_create
#define pthread_key_delete gesher_pthread_key_delete

// A signal sent to a thread is pending for the process, where sigwait takes
// it (see signal.cc); winpthreads' pthread_kill only checks the thread.
int gesher_pthread_kill(pthread_t thread, int sig);
#undef pthread_kill
#define pthread_kill gesher_pthread_kill

// winpthreads returns the CRT's errno numbers (ETIMEDOUT is 138, not 110);
// these translate the results that can differ from Linux's (see <errno.h>).
#define GESHER_PTHREAD_LINUX_ERRNO(name, params, args)                       static inline int gesher_##name params {                                    int result = name args;                                                   return result == 0 ? 0 : gesher_errno_from_crt(result);                 }
GESHER_PTHREAD_LINUX_ERRNO(pthread_join, (pthread_t t, void** res), (t, res))
GESHER_PTHREAD_LINUX_ERRNO(pthread_mutex_lock, (pthread_mutex_t* m), (m))
GESHER_PTHREAD_LINUX_ERRNO(pthread_mutex_trylock, (pthread_mutex_t* m), (m))
GESHER_PTHREAD_LINUX_ERRNO(pthread_mutex_timedlock,
                           (pthread_mutex_t* m, const struct timespec* ts), (m, ts))
GESHER_PTHREAD_LINUX_ERRNO(pthread_cond_timedwait,
                           (pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* ts),
                           (c, m, ts))
GESHER_PTHREAD_LINUX_ERRNO(pthread_rwlock_timedrdlock,
                           (pthread_rwlock_t* l, const struct timespec* ts), (l, ts))
GESHER_PTHREAD_LINUX_ERRNO(pthread_rwlock_timedwrlock,
                           (pthread_rwlock_t* l, const struct timespec* ts), (l, ts))
#undef GESHER_PTHREAD_LINUX_ERRNO
#undef pthread_join
#undef pthread_mutex_lock
#undef pthread_mutex_trylock
#undef pthread_mutex_timedlock
#undef pthread_cond_timedwait
#undef pthread_rwlock_timedrdlock
#undef pthread_rwlock_timedwrlock
#define pthread_join gesher_pthread_join
#define pthread_mutex_lock gesher_pthread_mutex_lock
#define pthread_mutex_trylock gesher_pthread_mutex_trylock
#define pthread_mutex_timedlock gesher_pthread_mutex_timedlock
#define pthread_cond_timedwait gesher_pthread_cond_timedwait
#define pthread_rwlock_timedrdlock gesher_pthread_rwlock_timedrdlock
#define pthread_rwlock_timedwrlock gesher_pthread_rwlock_timedwrlock

#ifdef __cplusplus
}
#endif
