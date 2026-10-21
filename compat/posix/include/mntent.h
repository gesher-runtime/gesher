// gesher-base: <mntent.h>. There is no mount table: setmntent returns a
// stream with no entries.
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNTED "/proc/mounts"

struct mntent {
  char* mnt_fsname;
  char* mnt_dir;
  char* mnt_type;
  char* mnt_opts;
  int mnt_freq;
  int mnt_passno;
};

FILE* setmntent(const char* filename, const char* type);
struct mntent* getmntent(FILE* stream);
struct mntent* getmntent_r(FILE* stream, struct mntent* mntbuf, char* buf, int buflen);
int endmntent(FILE* stream);
char* hasmntopt(const struct mntent* mnt, const char* opt);

#ifdef __cplusplus
}
#endif
