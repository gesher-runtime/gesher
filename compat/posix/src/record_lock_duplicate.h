// Internal CRT descriptor reservation. No guest-visible extra API.
#pragma once

namespace gesher::record_locks {

// Explicit unit-fixture dependencies, never selected by environment, guest
// input or the public fcntl entry. A successful close callback MUST perform
// the actual close; failure leaves the real owned resource observable.
struct DuplicateOperations {
  int (*set_inherit)(int descriptor, void* native_handle, bool inherit) = nullptr;
  int (*close_nul)(int descriptor) = nullptr;
  int (*close_unpublished)(int descriptor) = nullptr;
};
struct PrivateReservationStatus { unsigned nul = 0, native_nul = 0, uncertain = 0; };
PrivateReservationStatus InspectPrivateReservations();  // Read-only internal diagnostic.

// Drain only identified private NUL reservations. Unknown source-alias
// cleanup cannot be retried by descriptor number: retain it and fail closed.
// Callers hold DescriptorGuard; results are Linux errno values.
int DrainPrivateReservations();
int DuplicateAtLeast(int descriptor, int minimum, bool close_on_exec,
                     const DuplicateOperations* operations = nullptr);

}  // namespace gesher::record_locks
