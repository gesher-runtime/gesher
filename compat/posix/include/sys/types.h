// gesher-base: <sys/types.h> with Linux-compatible types on top of mingw's.
#pragma once

// Linux pid_t is 32 bits; mingw's is 64 bits on Win64. AOSP static_asserts
// the Linux size, so define it before mingw does.
#ifndef _PID_T_
#define _PID_T_
typedef int _pid_t;
typedef _pid_t pid_t;
#endif

#include_next <sys/types.h>

typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int id_t;
typedef unsigned int nlink_t;
typedef long long blkcnt_t;
typedef long long blksize_t;

// mingw only exposes sigset_t under _POSIX.
#ifndef _POSIX
typedef _sigset_t sigset_t;
#endif
