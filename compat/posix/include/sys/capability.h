// gesher-base: <sys/capability.h>. Windows processes have no Linux
// capabilities; capget reports none, capset fails with EPERM.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define _LINUX_CAPABILITY_VERSION_1 0x19980330
#define _LINUX_CAPABILITY_U32S_1 1
#define _LINUX_CAPABILITY_VERSION_2 0x20071026
#define _LINUX_CAPABILITY_U32S_2 2
#define _LINUX_CAPABILITY_VERSION_3 0x20080522
#define _LINUX_CAPABILITY_U32S_3 2

typedef struct __user_cap_header_struct {
  uint32_t version;
  int pid;
} *cap_user_header_t;

typedef struct __user_cap_data_struct {
  uint32_t effective;
  uint32_t permitted;
  uint32_t inheritable;
} *cap_user_data_t;

#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_RAW          13
#define CAP_SYS_ADMIN        21
#define CAP_LAST_CAP         40

int capget(cap_user_header_t header, cap_user_data_t data);
int capset(cap_user_header_t header, const cap_user_data_t data);

#ifdef __cplusplus
}
#endif
