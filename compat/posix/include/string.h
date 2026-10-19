// gesher-base: POSIX additions to <string.h>.
#pragma once

#include_next <string.h>

#ifdef __cplusplus
extern "C" {
#endif

char* strsignal(int sig);

#ifdef __cplusplus
}
#endif
