// Gesher: what AOSP's media sources take from bionic or their includes'
// includes that llvm-mingw's headers do not give them (force-included by
// cmake/aosp/media_codecs.cmake).
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
#include <new>
#include <vector>
#endif

// bionic's vasprintf and strcasestr (AString, ADebug).
static inline int gesher_vasprintf(char** out, const char* fmt, va_list ap) {
  va_list copy;
  va_copy(copy, ap);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (n < 0) {
    *out = NULL;
    return -1;
  }
  *out = (char*)malloc((size_t)n + 1);
  if (*out == NULL) return -1;
  return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}
#define vasprintf gesher_vasprintf

static inline char* gesher_strcasestr(const char* haystack, const char* needle) {
  size_t n = strlen(needle);
  for (; *haystack != '\0'; haystack++) {
    if (strncasecmp(haystack, needle, n) == 0) return (char*)haystack;
  }
  return n == 0 ? (char*)haystack : NULL;
}
#define strcasestr gesher_strcasestr

// bionic's memrchr (OggExtractor).
static inline void* gesher_memrchr(const void* s, int c, size_t n) {
  const unsigned char* p = (const unsigned char*)s;
  while (n-- > 0) {
    if (p[n] == (unsigned char)c) return (void*)(p + n);
  }
  return NULL;
}
#define memrchr gesher_memrchr

// PacketVideo's unused-argument macro (mp3dec's sources take it from oscl).
#ifndef OSCL_UNUSED_ARG
#define OSCL_UNUSED_ARG(x) ((void)(x))
#endif
