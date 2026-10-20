// gesher-base: Linux additions to <sched.h>.
#pragma once

#include_next <sched.h>

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000
#define CLONE_NEWNS   0x00020000

#define CPU_SETSIZE 1024
typedef struct {
  unsigned long long __bits[CPU_SETSIZE / 64];
} cpu_set_t;

#define CPU_ZERO(set) __builtin_memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, set) ((set)->__bits[(cpu) / 64] |= 1ULL << ((cpu) % 64))
#define CPU_CLR(cpu, set) ((set)->__bits[(cpu) / 64] &= ~(1ULL << ((cpu) % 64)))
#define CPU_ISSET(cpu, set) (((set)->__bits[(cpu) / 64] >> ((cpu) % 64)) & 1)
#define CPU_COUNT(set) gesher_cpu_count(set)

int gesher_cpu_count(const cpu_set_t* set);
int sched_getcpu(void);
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t* set);
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set);
int unshare(int flags);
int sched_getparam(pid_t pid, struct sched_param* param);
int sched_setparam(pid_t pid, const struct sched_param* param);

#ifdef __cplusplus
}
#endif
