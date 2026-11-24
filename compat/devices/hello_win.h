// Gesher (devices track): Windows Hello user-consent verification through its
// ABI (Windows.Security.Credentials.UI.UserConsentVerifier and
// IUserConsentVerifierInterop), for com.gesher.server.WindowsHello (ADR 0091,
// docs/research/windows-hello-native-spec.md). Nothing here reports success
// without Windows' own Verified.
#pragma once

#include <windows.h>
#include <stdint.h>

#include "winrt.h"

namespace gesher::devices::hello {

// UserConsentVerifierAvailability: Available 0, DeviceNotPresent 1,
// NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4; -1 when the
// query failed (*hr says why).
int Availability(HRESULT* hr);

// The foreground top-level window when process `pid` owns it, else null: the
// owner of a prompt shown for that process (also used by hello_key_win.cc).
HWND ForegroundWindowOf(DWORD pid);

// The window Start would make the prompt's owner for process `pid` right now:
// the root owner of the foreground window when that window is `pid`'s, else
// null. WindowsHello watches it while the prompt shows (hello_owner.h).
HWND OwnerWindow(DWORD pid);

// Why Start returned no verification.
enum class StartFailure { kNone, kNotForeground, kWindows };

struct Verification;

// Shows Windows Hello's prompt owned by the foreground window of process `pid`
// (the caller must be in the foreground, as SystemUI requires). `message` is
// UTF-16, at most 1024 units are used. Null on failure, with *failure and *hr.
Verification* Start(DWORD pid, const wchar_t* message, size_t length, StartFailure* failure,
                    HRESULT* hr);

// How long Await keeps waiting for Windows to end a prompt after Cancel.
constexpr DWORD kCancelGraceMs = 5000;

// Blocks until the prompt ends. UserConsentVerificationResult: Verified 0,
// DeviceNotPresent 1, NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4,
// RetriesExhausted 5, Canceled 6; -1 on a Windows error (logged).
//
// After Cancel the wait is bounded: when Windows has not ended the operation
// within `cancel_grace_ms` (its Cancel failed, or it ignores the request), the
// operation is abandoned and Await returns Canceled. The caller then calls
// Release as usual, which drops Gesher's references; a late result from the
// abandoned operation is never read, so it can never be reported.
int Await(Verification* v, DWORD cancel_grace_ms = kCancelGraceMs);

// Thread-safe and idempotent: asks Windows to cancel the prompt and starts
// Await's grace period, also when Windows refuses the request.
void Cancel(Verification* v);

// Releases the verification once Await has returned.
void Release(Verification* v);

namespace internal {

// Takes ownership of a started IAsyncOperation<UserConsentVerificationResult>
// (releases it on failure). Start uses it; hello_windows_test.cc passes
// scripted operations to exercise completion and cancellation without UI.
Verification* Wrap(winrt::IAsyncOperation<int>* operation, HRESULT* hr);

}  // namespace internal

}  // namespace gesher::devices::hello
