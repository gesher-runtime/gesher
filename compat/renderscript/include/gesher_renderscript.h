// Gesher: included before every C++ source of libRS and of librs_jni
// (cmake/aosp/renderscript.cmake, docs/renderscript.md): what bionic declares
// for them and mingw does not.
#pragma once

#include <stddef.h>
#include <stdint.h>

// <android/versioning.h>: an availability attribute for Android targets. A
// function-like macro, which CMake's compile definitions cannot carry.
#ifndef __DEPRECATED_IN
#define __DEPRECATED_IN(...)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// libcutils' strlcpy.c (bionic's <string.h>).
size_t strlcpy(char* dst, const char* src, size_t size);
// <malloc.h>: over gesher-base's posix_memalign, so free() takes the memory
// (compat/renderscript/rs_compat.cc).
void* memalign(size_t alignment, size_t size);
// <stdlib.h>: a random number from the system's generator
// (compat/renderscript/rs_compat.cc).
uint32_t arc4random(void);
// <math.h>: the reentrant lgammaf (compat/renderscript/rs_compat.cc).
float lgammaf_r(float x, int* sign);

#ifdef __cplusplus
}
#endif
