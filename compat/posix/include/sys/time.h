// gesher-base: utimes and friends, added to mingw's <sys/time.h>
// (src/path.cc).
#pragma once

#include_next <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

int utimes(const char* path, const struct timeval times[2]);
int lutimes(const char* path, const struct timeval times[2]);
int futimes(int fd, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif
