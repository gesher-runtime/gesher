// gesher-base: <sys/cdefs.h>, which glibc and bionic have and mingw-w64 may
// not. Android's NDK headers (frameworks/native's <android/*.h>) include it
// for __BEGIN_DECLS, __END_DECLS and __INTRODUCED_IN.
#pragma once

#if __has_include_next(<sys/cdefs.h>)
#include_next <sys/cdefs.h>
#endif

#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif

#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#endif
