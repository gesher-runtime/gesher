// gesher-base: <sys/ioctl.h>. Device ioctls do not exist on Windows; ioctl
// fails with ENOTTY.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD 0x541B
#define FIONBIO  0x5421

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
