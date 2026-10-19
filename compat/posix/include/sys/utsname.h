// gesher-base: <sys/utsname.h>. sysname is "Windows" and release is the
// Windows version, so Linux kernel-version checks fail safely.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

int uname(struct utsname* buf);

#ifdef __cplusplus
}
#endif
