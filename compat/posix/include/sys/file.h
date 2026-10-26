// gesher-base: <sys/file.h>, flock(2) over LockFileEx (src/system.cc,
// docs/posix.md, ADR 0057).
#pragma once

#include <fcntl.h>

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#ifdef __cplusplus
extern "C" {
#endif

// Advisory whole-file locks, as Linux's: one lock per open file (shared by
// its dup()s), held until LOCK_UN or the last descriptor of the open file is
// closed; converting a lock first drops the old one. Not visible to fcntl's
// record locks.
int flock(int fd, int operation);

#ifdef __cplusplus
}
#endif
