// gesher-base: <sys/mount.h>. Mount namespaces do not exist on Windows; the
// calls fail with ENOSYS.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define MS_RDONLY  1
#define MS_NOSUID  2
#define MS_NODEV   4
#define MS_NOEXEC  8
#define MS_REMOUNT 32
#define MS_BIND    4096
#define MS_REC     16384
#define MS_PRIVATE (1 << 18)
#define MS_SLAVE   (1 << 19)
#define MNT_DETACH 2

int mount(const char* source, const char* target, const char* type, unsigned long flags,
          const void* data);
int umount2(const char* target, int flags);

#ifdef __cplusplus
}
#endif
