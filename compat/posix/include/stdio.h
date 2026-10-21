// gesher-base: POSIX additions to <stdio.h> (src/misc.cc).
#pragma once

#include_next <stdio.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t getline(char** lineptr, size_t* n, FILE* stream);
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream);

#ifdef __cplusplus
}
#endif
