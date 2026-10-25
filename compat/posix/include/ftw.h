// gesher-base: <ftw.h>, file tree walks over opendir and lstat (src/ftw.cc).
#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTW_F   0  // a file
#define FTW_D   1  // a directory, before its contents
#define FTW_DNR 2  // a directory that cannot be read
#define FTW_NS  3  // stat failed
#define FTW_SL  4  // a symbolic link (FTW_PHYS)
#define FTW_DP  5  // a directory, after its contents (FTW_DEPTH)
#define FTW_SLN 6  // a symbolic link to nothing

#define FTW_PHYS          1
#define FTW_MOUNT         2
#define FTW_CHDIR         4
#define FTW_DEPTH         8
#define FTW_ACTIONRETVAL 16

// FTW_ACTIONRETVAL's return values.
#define FTW_CONTINUE      0
#define FTW_STOP          1
#define FTW_SKIP_SUBTREE  2
#define FTW_SKIP_SIBLINGS 3

struct FTW {
  int base;   // offset of the name in the path
  int level;  // depth below the starting directory
};

int ftw(const char* dir, int (*fn)(const char* path, const struct stat* sb, int flag), int nopenfd);
int nftw(const char* dir,
         int (*fn)(const char* path, const struct stat* sb, int flag, struct FTW* ftw),
         int nopenfd, int flags);
#define ftw64 ftw
#define nftw64 nftw

#ifdef __cplusplus
}
#endif
