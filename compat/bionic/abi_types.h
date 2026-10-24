// gesher bionic shim: bionic's LP64 ABI types, as the app's .so sees them.
//
// The .so was compiled against bionic, where `long` is 64-bit; the shim is
// compiled with mingw, where `long` is 32-bit. So every type that crosses the
// boundary is spelled with a fixed width here, independent of the compiler's
// `long`. Struct sizes match bionic's (bits/pthread_types.h) so the .so's
// storage for a pthread_mutex_t and the shim's view of it agree.
#pragma once

#include <stdint.h>

namespace gesher::bionic {

// System V, like all loaded code (docs/decisions/0006-managed-abi.md).
#define BIONIC_SYSV __attribute__((sysv_abi))

// pthread_t is `unsigned long` under bionic LP64: a 64-bit opaque id.
using bionic_pthread_t = uint64_t;
using bionic_pthread_key_t = int32_t;
using bionic_pthread_once_t = int32_t;

// bits/pthread_types.h, __LP64__ arms.
struct bionic_pthread_mutex_t {
  int32_t __private[10];
};
struct bionic_pthread_cond_t {
  int32_t __private[12];
};
struct bionic_pthread_rwlock_t {
  int32_t __private[14];
};
struct bionic_pthread_attr_t {
  uint32_t flags;
  void* stack_base;
  uint64_t stack_size;
  uint64_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
  char __reserved[16];
};
using bionic_pthread_mutexattr_t = int64_t;
using bionic_pthread_condattr_t = int64_t;
using bionic_pthread_rwlockattr_t = int64_t;

}  // namespace gesher::bionic
