// gesher-base: <pthread.h> additions.

#include <pthread.h>

#include <errno.h>
#include <time.h>
#include <windows.h>

extern "C" {

int pthread_getattr_np(pthread_t thread, pthread_attr_t* attr) {
  if (!pthread_equal(thread, pthread_self())) return ENOTSUP;
  ULONG_PTR low = 0;
  ULONG_PTR high = 0;
  GetCurrentThreadStackLimits(&low, &high);
  int rc = pthread_attr_init(attr);
  if (rc != 0) return rc;
  return pthread_attr_setstack(attr, reinterpret_cast<void*>(low), high - low);
}

int pthread_getcpuclockid(pthread_t thread, clockid_t* clock_id) {
  (void)thread;
  *clock_id = CLOCK_THREAD_CPUTIME_ID;
  return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t* attr, size_t* guardsize) {
  (void)attr;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  *guardsize = si.dwPageSize;
  return 0;
}

int pthread_attr_setguardsize(pthread_attr_t* attr, size_t guardsize) {
  (void)attr;
  (void)guardsize;
  return 0;
}

}  // extern "C"
