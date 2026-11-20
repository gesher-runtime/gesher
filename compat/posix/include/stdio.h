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

// These share Gesher's stream lock and descriptor-backed buffers. The CRT's
// _nolock functions operate on different internal buffers and are not aliases.
void flockfile(FILE* stream);
void funlockfile(FILE* stream);
int ftrylockfile(FILE* stream);
size_t fread_unlocked(void* bytes, size_t size, size_t count, FILE* stream);
size_t fwrite_unlocked(const void* bytes, size_t size, size_t count, FILE* stream);
int fgetc_unlocked(FILE* stream);
int getc_unlocked(FILE* stream);
int getchar_unlocked(void);
int fputc_unlocked(int value, FILE* stream);
int putc_unlocked(int value, FILE* stream);
int putchar_unlocked(int value);
char* fgets_unlocked(char* bytes, int count, FILE* stream);
int fputs_unlocked(const char* text, FILE* stream);
int fflush_unlocked(FILE* stream);
int ferror_unlocked(FILE* stream);
int feof_unlocked(FILE* stream);
void clearerr_unlocked(FILE* stream);

// Internal stream-lifetime bridge. The shared stream owner retires its
// metadata before releasing the native FILE token. An error before close
// leaves retired zero; errors after close/reinitialization still set it to one.
// Callers use this result to retain or reap associated process resources.
int gesher_fclose_state(FILE* stream, int* retired);
FILE* gesher_freopen_state(const char* path, const char* mode, FILE* stream, int* retired);

#ifdef __cplusplus
}
#endif
