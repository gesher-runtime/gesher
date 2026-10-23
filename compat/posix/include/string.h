// gesher-base: POSIX additions to <string.h>.
#pragma once

#include_next <string.h>

#ifdef __cplusplus
extern "C" {
#endif

char* strsignal(int sig);

// <strings.h>'s bzero, which glibc's and bionic's <string.h> declare too.
static inline void bzero(void* s, size_t n) {
  memset(s, 0, n);
}

// Messages for Linux errno values (see <errno.h>), in place of the CRT's.
char* gesher_strerror(int errnum);
int gesher_strerror_s(char* buf, size_t buflen, int errnum);
#define strerror gesher_strerror
#define strerror_s gesher_strerror_s

// GNU strerror_r; __xpg_strerror_r is the XSI one.
char* strerror_r(int errnum, char* buf, size_t buflen);
int __xpg_strerror_r(int errnum, char* buf, size_t buflen);

#ifdef __cplusplus
}
#endif
