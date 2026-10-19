// gesher-base: POSIX additions to mingw's <fcntl.h>.
//
// TODO(gesher): file descriptors still come from the CRT (_open), which
// lacks Linux semantics such as deleting or renaming files that are open.
// A gesher-base open() over CreateFileW is planned.
#pragma once

#include_next <fcntl.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC O_NOINHERIT
#endif
// Flags with no CRT equivalent. open() in gesher-base strips them before
// calling the CRT.
#define O_NONBLOCK  0x00100000
#define O_NDELAY    O_NONBLOCK
#define O_NOCTTY    0x00200000
#define O_DIRECTORY 0x00400000
#define O_NOFOLLOW  0x00800000
#define O_SYNC      0x01000000
#define O_DSYNC     O_SYNC
#define O_NOATIME   0x02000000
#define O_PATH      0x04000000
#define O_LARGEFILE 0

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
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
#define AT_EMPTY_PATH       0x1000

#define POSIX_FADV_NORMAL     0
#define POSIX_FADV_RANDOM     1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED   3
#define POSIX_FADV_DONTNEED   4

struct flock {
  short l_type;
  short l_whence;
  off_t l_start;
  off_t l_len;
  pid_t l_pid;
};

int fcntl(int fd, int cmd, ...);
int openat(int dirfd, const char* path, int flags, ...);
int posix_fadvise(int fd, off_t offset, off_t len, int advice);
int fallocate(int fd, int mode, off_t offset, off_t len);
int posix_fallocate(int fd, off_t offset, off_t len);

#ifdef __cplusplus
}
#endif
