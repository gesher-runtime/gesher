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

// Process creation is not supported (Android apps cannot fork either); these
// fail with ENOSYS.
pid_t fork(void);
int execv(const char* path, char* const argv[]);
int execve(const char* path, char* const argv[], char* const envp[]);
int execvp(const char* file, char* const argv[]);

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
unsigned int alarm(unsigned int seconds);
int nice(int inc);

#ifdef __cplusplus
}
#endif
