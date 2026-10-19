// gesher-base: <sys/resource.h>.
#pragma once

#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long rlim_t;
#define RLIM_INFINITY (~0ULL)

struct rlimit {
  rlim_t rlim_cur;
  rlim_t rlim_max;
};

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9

struct rusage {
  struct timeval ru_utime;
  struct timeval ru_stime;
  long long ru_maxrss;
  long long ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap;
  long long ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals;
  long long ru_nvcsw, ru_nivcsw;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2
#define PRIO_MIN     (-20)
#define PRIO_MAX     20

int getrlimit(int resource, struct rlimit* rlim);
int setrlimit(int resource, const struct rlimit* rlim);
int getrusage(int who, struct rusage* usage);
// On Linux (and Android) PRIO_PROCESS with a thread id sets that thread's
// nice value; Gesher maps nice values onto Windows thread priorities.
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

#ifdef __cplusplus
}
#endif
