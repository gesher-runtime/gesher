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

// libcap's interface (src/libcap.cc), as ART's tests use it: the process's
// sets are empty, so cap_set_proc succeeds only for empty sets (EPERM).
typedef struct gesher_cap* cap_t;
typedef int cap_value_t;
typedef enum { CAP_EFFECTIVE = 0, CAP_PERMITTED = 1, CAP_INHERITABLE = 2 } cap_flag_t;
typedef enum { CAP_CLEAR = 0, CAP_SET = 1 } cap_flag_value_t;
cap_t cap_init(void);
cap_t cap_get_proc(void);
cap_t cap_dup(cap_t cap);
int cap_free(void* object);
int cap_clear(cap_t cap);
int cap_clear_flag(cap_t cap, cap_flag_t flag);
int cap_get_flag(cap_t cap, cap_value_t value, cap_flag_t flag, cap_flag_value_t* out);
int cap_set_flag(cap_t cap, cap_flag_t flag, int count, const cap_value_t* values,
                 cap_flag_value_t set);
int cap_set_proc(cap_t cap);

#ifdef __cplusplus
}
#endif
