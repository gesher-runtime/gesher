// gesher-base: POSIX additions to mingw's <unistd.h>.
#pragma once

#include_next <unistd.h>

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _SC_CLK_TCK          2
#define _SC_OPEN_MAX         4
#define _SC_PAGESIZE         30
#define _SC_PAGE_SIZE        _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES       85

long sysconf(int name);
int getpagesize(void);

// Android apps run as a single uid; Gesher reports a fixed app uid.
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
pid_t getppid(void);
pid_t gettid(void);
pid_t setsid(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);

// Android apps cannot fork; neither can Gesher. Fails with ENOSYS.
// (execv and friends come from the Windows CRT.)
pid_t fork(void);

int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int fsync(int fd);
int fdatasync(int fd);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset);
ssize_t pread64(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite64(int fd, const void* buf, size_t count, off_t offset);
ssize_t readlink(const char* path, char* buf, size_t size);
int symlink(const char* target, const char* linkpath);
int link(const char* oldpath, const char* newpath);
int nice(int inc);

// The CRT's rmdir reports ENOTEMPTY with its own number (see <errno.h>).
int gesher_rmdir(const char* path);
#define rmdir gesher_rmdir

#ifdef __cplusplus
}
#endif
