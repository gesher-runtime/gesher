// Gesher target clocks extend the supplier's fixed POSIX clocks.
#pragma once
#include_next <time.h>
#ifdef __cplusplus
extern "C" {
#endif
int gesher_clock_gettime(clockid_t clock, struct timespec* value);
int gesher_clock_getres(clockid_t clock, struct timespec* value);
#ifdef __cplusplus
}
#endif
#define clock_gettime gesher_clock_gettime
#define clock_getres gesher_clock_getres
