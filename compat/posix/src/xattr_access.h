#pragma once

#include <windows.h>

// Private production authorization primitives, also exercised by the native
// fixture. Neither function selects a test policy or changes the caller's token.
namespace gesher::xattr {

// Opens the held object itself using the filesystem's current-token check.
// The optional observation receives the actual NtOpenFile status (or
// STATUS_NOT_IMPLEMENTED if the entry point is unavailable). Ownership of a
// successful result passes to the caller. No manual fallback is performed here.
int ReopenFileAccess(HANDLE file, DWORD desired, HANDLE* result, LONG* native_status = nullptr);

// Evaluates the supported simple security descriptor on the held object.
// Production may use this only after STATUS_DELETE_PENDING and only when the
// admitted handle already has the required native EA and READ_CONTROL rights.
// This routine itself neither opens a handle nor reads or writes any file EA.
int CheckHeldFileAccess(HANDLE file, DWORD desired);

}  // namespace gesher::xattr
