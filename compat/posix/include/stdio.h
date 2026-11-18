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

// Internal stream-lifetime bridge. The caller holds the CRT FILE lock while
// observing retired, so no allocator can reuse its slot before associated
// runtime metadata is removed. An error before close leaves retired zero;
// errors after close/reinitialization still set it to one.
int gesher_fclose_state(FILE* stream, int* retired);
FILE* gesher_freopen_state(const char* path, const char* mode, FILE* stream, int* retired);

#ifdef __cplusplus
}
#endif
