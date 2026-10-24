/* Gesher: what Tremolo's os_types.h takes from Cygwin's _G_config.h under
 * _WIN32 with GCC-style compilers (llvm-mingw): the fixed-width types. */
#pragma once
#include <stdint.h>
typedef int64_t _G_int64_t;
typedef int32_t _G_int32_t;
typedef uint32_t _G_uint32_t;
typedef int16_t _G_int16_t;
typedef uint16_t _G_uint16_t;

/* Tremolo's os_types.h only typedefs ogg_uint8_t on the non-_WIN32 path
 * (config_types.h); codec_internal.h uses it everywhere. */
typedef unsigned char ogg_uint8_t;
