// gesher-base: <dirent.h> with the Linux dirent layout (d_type included),
// replacing mingw's. Implemented over FindFirstFileW in compat/posix/src.
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
  unsigned long long d_ino;
  long long d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256 * 3];  // UTF-8 of a 255-character Windows name
};
#define dirent64 dirent

typedef struct gesher_DIR DIR;

DIR* opendir(const char* name);
DIR* fdopendir(int fd);
struct dirent* readdir(DIR* dir);
int closedir(DIR* dir);
void rewinddir(DIR* dir);
int dirfd(DIR* dir);
#define readdir64 readdir

#ifdef __cplusplus
}
#endif
