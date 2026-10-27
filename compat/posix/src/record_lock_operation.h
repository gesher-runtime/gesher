// Internal regular-file nonblocking lock operation. The descriptor/lifetime
// layer owns a stable FILE_OBJECT anchor and a validated registry transaction.
// This module does not install fcntl symbols or supply a missing owner token.
#pragma once
#include "record_lock_liveness.h"
#include <cstddef>

namespace gesher::record_locks {

struct LinuxFlock {
  int16_t type, whence;
  int64_t start, length;
  int32_t pid;
};
static_assert(sizeof(LinuxFlock) == 32);
static_assert(offsetof(LinuxFlock, start) == 8 && offsetof(LinuxFlock, length) == 16 &&
              offsetof(LinuxFlock, pid) == 24);

// Returns a Linux error number, or0 on success. Caller retains its fd lock,
// anchor and registry transaction throughout; commits only a successful
// mutation and restores the public function's success errno. The state must
// have passed Registry::Begin validation/pruning; a description owner must
// already have a bound live reference before a mutation is committed.
// Blocking waits, signal dispatch and descriptor publication live above this
// operation; they cannot be implemented by retrying a failed owner identity.
int NonblockingOperation(RegistryState* state, HANDLE anchor, const Owner& owner,
                         int command, void* user_flock, bool* mutated);

}  // namespace gesher::record_locks
