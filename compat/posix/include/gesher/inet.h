// gesher-base: Android's INTERNET permission (docs/network.md).
//
// On Android an app without android.permission.INTERNET runs without the inet
// group: creating an AF_INET or AF_INET6 socket fails with EACCES, and so does
// a name lookup (EAI_SYSTEM with errno EACCES, which libcore reports as
// "Permission denied (missing INTERNET permission?)"). Gesher's app processes
// get the same: GESHER_INET=0 in the environment (the system server sets it
// for packages that do not request INTERNET), or gesher_set_inet_allowed(0).
// Loopback socketpairs and AF_UNIX sockets are not affected.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void gesher_set_inet_allowed(int allowed);
int gesher_inet_allowed(void);

#ifdef __cplusplus
}
#endif
