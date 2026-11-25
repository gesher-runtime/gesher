// The fork set's interface for host fixtures that compile the ELF loader's
// production files alone (gesher/fork.h, ADR 0092). These fixtures never
// fork: a leaving thread's cleanup always takes its lock, and the files'
// fork-set registrations (#ifdef GESHER_FORK_LOCK) are left out.
#pragma once

namespace gesher {
template <typename Lock>
bool LockUnlessForkHolds(Lock& lock) {
  lock.lock();
  return true;
}
}  // namespace gesher
