// Gesher (devices track): Windows Hello user-consent verification through its
// ABI (Windows.Security.Credentials.UI.UserConsentVerifier and
// IUserConsentVerifierInterop), for com.gesher.server.WindowsHello (ADR 0091,
// docs/research/windows-hello-native-spec.md). Nothing here reports success
// without Windows' own Verified.
#pragma once

#include <windows.h>
#include <stdint.h>

namespace gesher::devices::hello {

// UserConsentVerifierAvailability: Available 0, DeviceNotPresent 1,
// NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4; -1 when the
// query failed (*hr says why).
int Availability(HRESULT* hr);

// The foreground top-level window when process `pid` owns it, else null: the
// owner of a prompt shown for that process (also used by hello_key_win.cc).
HWND ForegroundWindowOf(DWORD pid);

// Why Start returned no verification.
enum class StartFailure { kNone, kNotForeground, kWindows };

struct Verification;

// Shows Windows Hello's prompt owned by the foreground window of process `pid`
// (the caller must be in the foreground, as SystemUI requires). `message` is
// UTF-16, at most 1024 units are used. Null on failure, with *failure and *hr.
Verification* Start(DWORD pid, const wchar_t* message, size_t length, StartFailure* failure,
                    HRESULT* hr);

// Blocks until the prompt ends. UserConsentVerificationResult: Verified 0,
// DeviceNotPresent 1, NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4,
// RetriesExhausted 5, Canceled 6; -1 on a Windows error (logged).
int Await(Verification* v);

// Thread-safe and idempotent: asks Windows to cancel the prompt.
void Cancel(Verification* v);

// Releases the verification once Await has returned.
void Release(Verification* v);

}  // namespace gesher::devices::hello
