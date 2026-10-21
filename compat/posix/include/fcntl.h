// gesher-base: <fcntl.h> with Linux's open flags.
//
// Android's Java libraries pass Linux's O_* numbers to native code
// (OsConstants.O_CREAT == 64), so open() and fcntl() take Linux values and
// gesher-base opens files with them (src/path.cc). The CRT's own names
// (_O_CREAT, _O_BINARY, ...) keep their values. See
// docs/decisions/0008-linux-abi.md.
#pragma once

#include_next <fcntl.h>

#include <sys/types.h>

#undef O_RDONLY
#undef O_WRONLY
#undef O_RDWR
#undef O_ACCMODE
#undef O_APPEND
#undef O_CREAT
#undef O_TRUNC
#undef O_EXCL
#undef O_TEXT
#undef O_BINARY
#undef O_RAW
#undef O_NOINHERIT
#undef O_TEMPORARY
#undef O_SHORT_LIVED
#undef O_SEQUENTIAL
#undef O_RANDOM

#define O_ACCMODE   00000003
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_NOCTTY    00000400
#define O_TRUNC     00001000
#define O_APPEND    00002000
#define O_NONBLOCK  00004000
#define O_NDELAY    O_NONBLOCK
#define O_DSYNC     00010000
#define O_ASYNC     00020000
#define O_DIRECT    00040000
#define O_LARGEFILE 00100000
#define O_DIRECTORY 00200000
#define O_NOFOLLOW  00400000
#define O_NOATIME   01000000
#define O_CLOEXEC   02000000
#define O_SYNC      04010000
#define O_RSYNC     O_SYNC
#define O_PATH      010000000
#define O_TMPFILE   020200000
// Files are always opened in binary mode; Windows code that asks for it
// explicitly still compiles.
#define O_BINARY    0
#define O_NOINHERIT O_CLOEXEC

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_GETLK64       F_GETLK
#define F_SETLK64       F_SETLK
#define F_SETLKW64      F_SETLKW
#define F_SETOWN        8
#define F_GETOWN        9
#define F_DUPFD_CLOEXEC 1030
#define F_ADD_SEALS     1033
#define F_GET_SEALS     1034

#define FD_CLOEXEC 1

#define F_SEAL_SEAL   0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW   0x0004
#define F_SEAL_WRITE  0x0008
#define F_SEAL_FUTURE_WRITE 0x0010

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000

#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4

#ifdef __cplusplus
extern "C" {
#endif

struct flock {
  short l_type;
  short l_whence;
  off_t l_start;
  off_t l_len;
  pid_t l_pid;
};
#define flock64 flock

int fcntl(int fd, int cmd, ...);
int openat(int dirfd, const char* path, int flags, ...);
int posix_fadvise(int fd, off_t offset, off_t len, int advice);
int fallocate(int fd, int mode, off_t offset, off_t len);
int posix_fallocate(int fd, off_t offset, off_t len);
#define open64 open
#define openat64 openat
#define fcntl64 fcntl

#ifdef __cplusplus
}
#endif
