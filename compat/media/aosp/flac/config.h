/* Gesher: libFLAC's config.h for Windows x64 with llvm-mingw, in place of
 * external/flac's (written for Linux, where it reads __LP64__ for pointer
 * sizes): integer-only, no assembly, no Ogg, as Android builds it. */
#pragma once
#define CPU_IS_BIG_ENDIAN 0
#define CPU_IS_LITTLE_ENDIAN 1
#define WORDS_BIGENDIAN 0
#define ENABLE_64_BIT_WORDS 1
#define FLAC__ALIGN_MALLOC_DATA 1
#define FLAC__HAS_NEONINTRIN 0
#define FLAC__HAS_OGG 0
#define FLAC__HAS_X86INTRIN 0
#define FLAC__NO_ASM 1
#define HAVE_INTTYPES_H 1
#define HAVE_LROUND 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define PACKAGE_VERSION "1.4.3"
#define VERSION "1.4.3"
#define SIZEOF_OFF_T 8
#define SIZEOF_VOIDP 8
