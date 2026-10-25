// gesher-base: POSIX additions to <stdio.h> (src/misc.cc).
#pragma once

#include_next <stdio.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t getline(char** lineptr, size_t* n, FILE* stream);
// rename, renameat and renameat2 replace the new name atomically, as Linux's
// do (src/fsops.cc). RENAME_EXCHANGE is not supported (EINVAL).
#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE  (1 << 1)
#define RENAME_WHITEOUT  (1 << 2)
int renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
              unsigned int flags);
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream);

#ifdef __cplusplus
}
#endif
