// gesher-base: POSIX additions to mingw's <sys/stat.h>.
#pragma once

#include_next <sys/stat.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef S_IFLNK
#define S_IFLNK  0xA000
#define S_IFSOCK 0xC000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
#ifndef S_IRWXG
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif

int lstat(const char* path, struct stat* buf);
int fchmod(int fd, mode_t mode);
int fchown(int fd, uid_t owner, gid_t group);
int mkfifo(const char* path, mode_t mode);
int fstatat(int dirfd, const char* path, struct stat* buf, int flags);
int mkdirat(int dirfd, const char* path, mode_t mode);
int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);

#ifdef __cplusplus
}

// POSIX mkdir takes a mode; Windows' takes only a path. Permissions are not
// modelled, so the mode is ignored.
inline int mkdir(const char* path, mode_t) { return mkdir(path); }
#endif
