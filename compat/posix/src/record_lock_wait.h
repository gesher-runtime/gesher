#pragma once
#include "record_lock_operation.h"

namespace gesher::record_locks {
// Internal finite POSIX-process operation, NOT an implementation of command7
// or38. registry is already Open(), private to this calling thread. prepared
// is an immutable PrepareOperation(..., F_SETLK, ...) snapshot of a pinned
// real file handle. Caller keeps that anchor alive and releases DescriptorGuard
// before entry. No descriptor number or user memory is reread during a wait.
// Caller owns a live manual-reset cancellation event for the whole call.
// Any original-fd close/replacement cancellation policy belongs to that caller;
// this primitive cannot attach itself to a reusable CRT fd number.
// Finite timeout1..60000ms; cancellation wins if observed before publication.
// Return0 means committed ownership; cancellation after publication cannot
// revoke it. Returns ECANCELED/ETIMEDOUT, never synthesizes EINTR/SA_RESTART.
// OFD/query/unlock are excluded. No public EDEADLK semantics are promised.
int WaitProcessLock(Registry& registry, const PreparedOperation& prepared,
                    HANDLE cancel, DWORD timeout);
// Best-effort post-publication hints under registry mutex. Never modifies
// registry contents and never changes the committed operation's result.
void NotifyRecordWaiters(const RegistryState& state);
}  // namespace gesher::record_locks
