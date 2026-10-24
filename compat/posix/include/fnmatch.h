// gesher-base: <fnmatch.h>, with Linux's flag values (src/fnmatch.cc).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FNM_NOMATCH 1

#define FNM_PATHNAME    0x01
#define FNM_NOESCAPE    0x02
#define FNM_PERIOD      0x04
#define FNM_LEADING_DIR 0x08
#define FNM_CASEFOLD    0x10
#define FNM_FILE_NAME   FNM_PATHNAME
#define FNM_IGNORECASE  FNM_CASEFOLD

int fnmatch(const char* pattern, const char* string, int flags);

#ifdef __cplusplus
}
#endif
