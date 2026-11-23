# Windows Hello backend: native specification (ADR 0091, phase 2)

What a Windows implementer builds so `com.gesher.server.AuthService` can confirm a
device credential. The Java side (AuthService, the `WindowsHelloBackend` interface,
the session rules and host tests) exists; Services.java passes `null` as the backend
until this lands. Nothing here may report success without Windows' own `Verified`.

Status of the facts below: enum values and the interop signature were checked against
Microsoft's documentation sources (`MicrosoftDocs/winrt-api`, `MicrosoftDocs/sdk-api`),
the SDK's `UserConsentVerifierInterop.h` and `windows.security.credentials.ui.idl`
(`microsoft/win32metadata`, RecompiledIdlHeaders). Parameterized IIDs were computed with
WinRT's algorithm, validated on `IAsyncOperation<bool>` =
`{cdb5efb3-5788-509d-9be1-71ccb8a3362a}`. Items marked **verify on Windows** were not
testable from Linux.

## 1. Files and build

| File | Content |
| --- | --- |
| `compat/devices/hello_win.h`, `hello_win.cc` | `gesher::devices::hello`: availability, start, await, cancel, release over the WinRT ABI (no C++/WinRT, no SDK WinRT headers, as `location_win.cc`). |
| `compat/devices/hello_jni.cc` | The JNI table for `com/gesher/server/WindowsHello`; `int register_gesher_hello(JNIEnv*)`. |
| `compat/devices/devices_jni.cc` | `register_gesher_devices` also calls `register_gesher_hello(env)`. |
| `compat/devices/devices.cmake` | add `hello_jni.cc` and `hello_win.cc` to `target_sources(framework_runtime PRIVATE ...)`. No new link libraries: combase functions are found at run time (`winrt.cc`), `user32` is already linked by the runtime. |
| `compat/devices/winrt.h/.cc` | export `HRESULT CreateString(const wchar_t* s, UINT32 length, HSTRING* out)` (today `Name` is private to winrt.cc). |
| `app/java/com/gesher/server/WindowsHello.java` | `final class WindowsHello implements WindowsHelloBackend` with the natives below and `static WindowsHello create()` returning null when `nativeAvailability` throws `UnsatisfiedLinkError` (as `WindowsHost.available()`). |
| `app/java/com/gesher/server/Services.java` | pass `WindowsHello.create()` instead of `null`. |

## 2. Windows interfaces (ABI)

```
// windows.security.credentials.ui.idl
[uuid(AF4F3F91-564C-4DDC-B8B5-973447627C65)]
interface IUserConsentVerifierStatics : IInspectable {
  HRESULT CheckAvailabilityAsync(IAsyncOperation<UserConsentVerifierAvailability>** result);
  HRESULT RequestVerificationAsync(HSTRING message,
                                   IAsyncOperation<UserConsentVerificationResult>** result); // UWP only
}
// UserConsentVerifierInterop.h, desktop partition, NTDDI_VERSION >= NTDDI_WIN10_RS3
MIDL_INTERFACE("39E050C3-4E74-441A-8DC0-B81104DF949C")
IUserConsentVerifierInterop : IInspectable {
  HRESULT RequestVerificationForWindowAsync(HWND appWindow, HSTRING message,
                                            REFIID riid, void** asyncOperation);
}
```

- Activation factory: `winrt::GetActivationFactory(L"Windows.Security.Credentials.UI.UserConsentVerifier", IID, ...)`
  once for `IUserConsentVerifierStatics`, and QueryInterface it (or a second
  `GetActivationFactory`) for `IUserConsentVerifierInterop`.
- `IAsyncOperation<UserConsentVerificationResult>` = `{fd596ffd-2318-558f-9dbe-d21df43764a5}`
  (pass as `riid`); `IAsyncOperation<UserConsentVerifierAvailability>` =
  `{ddd384f3-d818-5d83-ab4b-32119c28587c}`. (Completed handlers, if used instead of
  polling: `{0cffc6c9-4c2b-5cd4-b38c-7b8df3ff5afb}` and `{28988174-ace2-5c15-a0df-580a26d94294}`.)
  Both results are 32-bit enums: `winrt::IAsyncOperation<int>::GetResults(int*)`.
- `UserConsentVerifierAvailability`: Available 0, DeviceNotPresent 1,
  NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4.
  `UserConsentVerificationResult`: Verified 0, DeviceNotPresent 1, NotConfiguredForUser 2,
  DisabledByPolicy 3, DeviceBusy 4, RetriesExhausted 5, Canceled 6.
- Minimum Windows: the interop header is guarded by `NTDDI_WIN10_RS3` (1709), while the
  docs page lists build 22000. **Verify on Windows 10** (22H2) that the interop factory
  QueryInterface succeeds; if it fails, `nativeAvailability` reports DeviceNotPresent with
  the HRESULT, and nothing is offered.

## 3. JNI surface (`com.gesher.server.WindowsHello`)

| Java | Signature | Native |
| --- | --- | --- |
| `static native int nativeAvailability(int[] hr)` | `([I)I` | `CheckAvailabilityAsync`, wait ≤ 2 s; returns 0..4, or `-1` (FAILED) with `hr[0]` set. |
| `static native long nativeStart(int pid, String message, int[] hr)` | `(ILjava/lang/String;[I)J` | Owner check + `RequestVerificationForWindowAsync`; returns a handle (`new Verification*`) or 0 with `hr[0]` and the reason. |
| `static native int nativeAwait(long handle)` | `(J)I` | Blocks until the operation leaves Started; returns 0..6, or -1. |
| `static native void nativeCancel(long handle)` | `(J)V` | Thread-safe: sets the cancel flag and calls `IAsyncInfo::Cancel`. Idempotent. |
| `static native void nativeRelease(long handle)` | `(J)V` | Releases the COM objects and deletes the handle; called once, after `nativeAwait` returned. |

Java side (`WindowsHello.verify`): `handle = nativeStart(...)`; if 0, post the start
failure's result (see table: CANCELED for "owner not in foreground", else FAILED) on a
new daemon thread (the interface forbids calling the completion before returning).
Otherwise start a daemon thread `auth-hello-<id>` that runs `r = nativeAwait(handle)`,
then, under a lock, marks the handle released and calls `nativeRelease`, then
`completion.onResult(r)`. `Verification.cancel()` calls `nativeCancel(handle)` under the
same lock unless released. No JNI callbacks into Java, no attached native threads.

## 4. Window ownership and foreground

- The verification runs in the system server; the prompt must be owned by the app's
  window, as Gesher's file and folder pickers already are (`devsvc_win32.cc` uses
  `GetForegroundWindow()` as the dialog owner).
- `nativeStart(pid, ...)`: `HWND w = GetForegroundWindow()`; require
  `GetWindowThreadProcessId(w, &owner)` with `owner == pid`, the process alive
  (`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE)` + `WaitForSingleObject(…, 0) == WAIT_TIMEOUT`)
  and `GetForegroundWindow() == w` again after the check (the same TOCTOU discipline as
  `ForegroundProcess` in `clipboard_access.cc`). Use `GetAncestor(w, GA_ROOTOWNER)` as
  `appWindow`. If the owner is not the caller: return 0 with reason "not foreground";
  Java reports `CANCELED` → `BIOMETRIC_ERROR_USER_CANCELED`, matching SystemUI's
  `cancelIfOwnerIsNotInForeground` (AuthService already fast-fails non-foreground callers;
  this covers the race).
- **Verify on Windows**: that a window of another process is accepted as `appWindow`
  (the Hello UI is hosted by Windows' credential broker, so a cross-process owner is
  expected to work; if `E_INVALIDARG`/`E_ACCESSDENIED`, the fallback is to run the call
  in the app's process through a Gesher-internal binder call, keeping the session in the
  system server).
- While the prompt is up, if the owner process exits, cancel (the Java side also
  cancels on binder death). Losing foreground during the prompt: Windows keeps its
  dialog modal to the owner; phase 2 may add a 250 ms foreground check in the await
  loop that cancels with reason "owner left foreground" → USER_CANCELED. Decide after
  observing Windows' own behavior.

## 5. Threading

- Every native entry calls `winrt::EnsureApartment()` (MTA). **Verify on Windows** that
  `RequestVerificationForWindowAsync` works from an MTA thread without a message loop on
  the calling thread; Microsoft's samples call it from UI threads. If it requires an STA,
  create one dedicated STA thread in hello_win.cc with a message loop (`MsgWaitForMultipleObjectsEx`)
  and marshal start/cancel to it; `nativeAwait` then waits on an event the STA sets.
- `nativeAwait` polls `IAsyncInfo::get_Status` (as `winrt::Wait`, without its timeout:
  a user may take minutes), with 1→20 ms backoff, or registers the completed handler
  and waits on an event. It must return when canceled.
- `nativeAvailability` is called on AuthService's thread (before each prompt) and, in
  phase 2, by TrustService.isDeviceSecure: cache the value for 5 s in Java, wait at most
  2 s natively, and treat FAILED as not available.

## 6. Cancellation

`nativeCancel` → `IAsyncInfo::Cancel()`; the operation then completes with status
Canceled (2) → `nativeAwait` returns 6 (CANCELED). AuthService has already told the app
`BIOMETRIC_ERROR_CANCELED` and drops this late result; a CANCELED it did not request is
the user's (`BIOMETRIC_ERROR_USER_CANCELED`). **Verify on Windows** that Cancel closes the
visible Hello dialog; if it does not, document that the dialog lingers and its eventual
result is ignored.

## 7. Error mapping (native → `WindowsHelloBackend` → Android)

| Native outcome | Backend value | Android (AuthService) |
| --- | --- | --- |
| Verified (0) | `VERIFIED` | `onAuthenticationSucceeded(AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL)` |
| Canceled (6) / status Canceled, not requested by the app | `CANCELED` | `onError(TYPE_CREDENTIAL, BIOMETRIC_ERROR_USER_CANCELED)` |
| Canceled after `cancelAuthentication` | `CANCELED` (dropped) | `BIOMETRIC_ERROR_CANCELED`, already sent |
| owner not in foreground at start | `CANCELED` | `BIOMETRIC_ERROR_USER_CANCELED` |
| RetriesExhausted (5) | `RETRIES_EXHAUSTED` | `BIOMETRIC_ERROR_LOCKOUT` |
| NotConfiguredForUser (2) | `NOT_CONFIGURED_FOR_USER` | `BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL` |
| DeviceNotPresent (1), DisabledByPolicy (3), DeviceBusy (4) | same | `BIOMETRIC_ERROR_HW_UNAVAILABLE` |
| combase missing (`E_NOTIMPL`), factory/QI failure, start HRESULT, status Error | `FAILED` | `BIOMETRIC_ERROR_HW_UNAVAILABLE` |
| availability ≠ Available before a prompt | (no prompt) | NotConfigured → `NO_DEVICE_CREDENTIAL`, else `HW_UNAVAILABLE` |

Log every HRESULT with the call that returned it (style of `fix(window): say which
Windows call refused a window`).

## 8. Message

`message` = the prompt's title, subtitle and description joined by newlines
(AuthService builds it). Convert from Java UTF-16 with `GetStringChars`, cap at 1024
UTF-16 units, create the HSTRING with `winrt::CreateString`, delete it after the call.
Windows shows its own title and buttons; the app's negative button cannot be shown
(credential-only prompts have none in Android either).

## 9. Tests

- Unit: a `GESHER_HELLO_TEST=1` switch below the JNI layer (as `bluetooth_test.cc`)
  returning scripted availability/results and honoring cancel, run in CI with the host
  JVM tests. Never compiled into a production path that is on by default.
- Windows by hand: a machine with Hello PIN only, PIN + fingerprint, and Hello not set
  up; app cancel while the dialog shows; app killed while the dialog shows; app moved to
  background; Windows 10 22H2 and Windows 11.
