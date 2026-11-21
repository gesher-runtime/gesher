// gesher-base: <sys/utsname.h>. sysname is "Linux", as on Android, and release
// is "0.0.0-gesher", so Linux kernel-version checks fail safely (system.cc).
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
