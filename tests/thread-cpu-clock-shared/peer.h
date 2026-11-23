#pragma once
#include <windows.h>
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif
struct clock_exit_gate {
  HANDLE reached;
  HANDLE release;
  volatile LONG error;
};
struct clock_peer_api {
  unsigned size;
  pthread_t (*self)(void);
  int (*create)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
  int (*join)(pthread_t, void**);
  int (*clock_id)(pthread_t, clockid_t*);
  int (*read)(clockid_t, struct timespec*);
  int (*resolution)(clockid_t, struct timespec*);
  int (*arm_exit_gate)(struct clock_exit_gate*);
};
typedef const struct clock_peer_api* (*clock_peer_entry)(void);
#ifdef __cplusplus
}
#endif
