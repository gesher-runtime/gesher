// Internal CRT descriptor reservation. No guest-visible extra API.
#pragma once

namespace gesher::record_locks {

// Drain only identified private NUL reservations. Unknown source-alias
// cleanup cannot be retried by descriptor number: retain it and fail closed.
// Callers hold DescriptorGuard; results are Linux errno values.
int DrainPrivateReservations();
int DuplicateAtLeast(int descriptor, int minimum, bool close_on_exec);

}  // namespace gesher::record_locks
