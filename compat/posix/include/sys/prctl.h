// gesher-base: <sys/prctl.h>. PR_SET_NAME/PR_GET_NAME name the calling
// thread; the dumpable, no-new-privs and bounding-set options report what a
// process without capabilities would; other options fail with EINVAL.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_PDEATHSIG  1
#define PR_GET_PDEATHSIG  2
#define PR_GET_DUMPABLE   3
#define PR_SET_DUMPABLE   4
#define PR_SET_NAME       15
#define PR_GET_NAME       16
#define PR_CAPBSET_READ   23
#define PR_CAPBSET_DROP   24
#define PR_SET_NO_NEW_PRIVS 38
#define PR_SET_VMA        0x53564d41
#define PR_SET_VMA_ANON_NAME 0

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif
