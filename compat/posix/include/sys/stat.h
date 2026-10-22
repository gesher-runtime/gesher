// gesher-base: <sys/stat.h> with Linux's struct stat.
//
// libcore reads st_atim/st_mtim/st_ctim (nanoseconds), st_blksize and
// st_blocks, which mingw's struct stat lacks; its st_ino and st_uid are 16
// bits. So mingw's struct stat and struct stat64, and the stat, fstat and
// wstat it declares for them, are renamed out of the way while its header is
// included, and gesher-base defines Linux's struct (x86-64 field order) with
// stat, fstat, lstat and fstatat over Win32 (src/stat.cc). As in glibc,
// st_atime/st_mtime/st_ctime are the seconds of the timespecs, and the *64
// names are the same struct and functions.
#pragma once

#include <io.h>
#include <direct.h>
#include <sys/types.h>
#include <time.h>

#define stat gesher_mingw_stat
#define stat64 gesher_mingw_stat64
#define fstat gesher_mingw_fstat
#define fstat64 gesher_mingw_fstat64
#define wstat gesher_mingw_wstat
#define wstat64 gesher_mingw_wstat64
#include_next <sys/stat.h>
#undef stat
#undef stat64
#undef fstat
#undef fstat64
#undef wstat
#undef wstat64

#ifdef __cplusplus
extern "C" {
#endif

struct stat {
  unsigned long long st_dev;    // the volume serial number
  unsigned long long st_ino;    // the file index
  unsigned long long st_nlink;
  unsigned int st_mode;
  uid_t st_uid;
  gid_t st_gid;
  int __pad0;
  unsigned long long st_rdev;
  long long st_size;
  long long st_blksize;
  long long st_blocks;          // 512-byte units
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;      // the last change to the file or its metadata
  long long __unused[3];
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define fstatat64 fstatat

int stat(const char* path, struct stat* buf);
int fstat(int fd, struct stat* buf);
int lstat(const char* path, struct stat* buf);
int fstatat(int dirfd, const char* path, struct stat* buf, int flags);

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

int fchmod(int fd, mode_t mode);
int fchown(int fd, uid_t owner, gid_t group);
int mkfifo(const char* path, mode_t mode);
// Device nodes cannot be created (EPERM); regular files can (S_IFREG).
int mknod(const char* path, mode_t mode, dev_t dev);
int mkdirat(int dirfd, const char* path, mode_t mode);
// Special tv_nsec values for utimensat and futimens.
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);

#ifdef __cplusplus
}

// POSIX mkdir takes a mode; Windows' takes only a path. Permissions are not
// modelled, so the mode is ignored.
inline int mkdir(const char* path, mode_t) { return mkdir(path); }
#else
// C has no overloads; <io.h> and <direct.h>, included above, have declared
// the one-argument mkdir already.
static inline int gesher_mkdir_mode(const char* path, mode_t mode) {
  (void)mode;
  return mkdir(path);
}
#define mkdir(path, mode) gesher_mkdir_mode(path, mode)
#endif
