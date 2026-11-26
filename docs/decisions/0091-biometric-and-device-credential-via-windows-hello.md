# 0091: Biometric and device-credential authentication over Windows Hello (auth track)

Status: proposed. Phase 1 (below) is implemented; the phases after it wait for the
scope questions at the end to be answered by maintainers.

## Context

Apps ask Android who the user is through four surfaces:

- `BiometricManager.canAuthenticate(authenticators)` and `BiometricManager.Strings`,
  `getLastAuthenticationTime` (the `auth` service, `IAuthService`);
- `BiometricPrompt.authenticate(...)`, with or without a `CryptoObject` (also `auth`);
- `KeyguardManager.isDeviceSecure()`, `isKeyguardSecure()` and
  `createConfirmDeviceCredentialIntent()` (the `trust` service and the window manager,
  and an activity for `ACTION_CONFIRM_DEVICE_CREDENTIAL`);
- `FingerprintManager` (deprecated since API 28, the `fingerprint` service) and keys
  created with `KeyGenParameterSpec.setUserAuthenticationRequired(true)` or
  `setUserAuthenticationParameters(...)`, which keystore2 releases only after an auth
  token for the user's secure user id.

Banking apps (Bank Leumi and Bank Hapoalim were reported by users) gate login and
transactions on these. Gesher at upstream `b3ede295` serves none of `auth`,
`biometric`, `fingerprint` or `face`; `trust` answers `isDeviceSecure=false` and
`isDeviceLocked=false` (DeviceServices.TrustService); `com.gesher.app.AndroidKeyStore`
keeps software keys and documents that user-authentication authorizations are not
enforced; no `FEATURE_FINGERPRINT`, `FEATURE_FACE` or `FEATURE_SECURE_LOCK_SCREEN` is
declared (PackageManagerService.FEATURES); SettingsIntents sends `BIOMETRIC_ENROLL` and
`FINGERPRINT_ENROLL` to `ms-settings:signinoptions`.

What apps observe today, from android-16.0.0_r4 sources (GitHub mirror
`msft-mirror-aosp/platform.frameworks.base`, tag `android-16.0.0_r4`):

- `SystemServiceRegistry` builds `BiometricManager` with
  `ServiceManager.getServiceOrThrow(Context.AUTH_SERVICE)`; with no `auth`,
  `getSystemService(BiometricManager.class)` is **null**. androidx.biometric
  (`BiometricManager.canAuthenticate`, API 30+) then answers
  `BIOMETRIC_ERROR_HW_UNAVAILABLE` ("BiometricManager was null").
- `BiometricPrompt.Builder.build()` takes
  `IAuthService.Stub.asInterface(ServiceManager.getService(AUTH_SERVICE))`, which is
  null; `authenticateInternal` then calls `mService.authenticate(...)` and the app gets a
  **NullPointerException** (only `RemoteException` is caught there).
- `KeyguardManager.createConfirmDeviceCredentialIntent` returns null because
  `isDeviceSecure()` is false.

A sensor-less AOSP device does not look like this. `SystemServer` starts
`BiometricService` and `AuthService` unconditionally (SystemServer.java, "Start this
service after all biometric sensor services are started"); only the per-modality
`FingerprintService`/`FaceService`/`IrisService` depend on `FEATURE_*`. So the
Android-defined "no biometric hardware" state is an `auth` service that answers
`BIOMETRIC_ERROR_NO_HARDWARE`, not a missing service.

## AOSP contracts (android-16.0.0_r4)

### `IAuthService` (AuthService.java, BiometricService.java, PreAuthInfo.java, Utils.java)

- Permission: `checkPermission()` accepts `USE_FINGERPRINT` or else enforces
  `USE_BIOMETRIC` (SecurityException); a `userId` other than the caller's enforces
  `USE_BIOMETRIC_INTERNAL`. `hasEnrolledBiometrics`, `registerEnabledOnKeyguardCallback`,
  the authentication-state and identity-check listeners, `invalidateAuthenticatorIds`,
  `resetLockout*` are `USE_BIOMETRIC_INTERNAL`; the test methods are `TEST_BIOMETRIC`;
  `getEnrollmentStatusList` is `SET_BIOMETRIC_DIALOG_ADVANCED`. `getAuthenticatorIds` for
  the caller's own user is deliberately unchecked (the in-app keystore calls it).
- Authenticator validation (`Utils.isValidAuthenticatorConfig`, called by
  `canAuthenticate`, `authenticate`, `getCurrentModality` and `getSupportedModalities`):
  `0` is valid; bits outside `DEVICE_CREDENTIAL | BIOMETRIC_MIN_STRENGTH |
  IDENTITY_CHECK` are invalid; the biometric bits must be none (with
  `DEVICE_CREDENTIAL`), `BIOMETRIC_STRONG` (0x0F) or `BIOMETRIC_WEAK` (0xFF); otherwise
  `IDENTITY_CHECK` requires `SET_BIOMETRIC_DIALOG_ADVANCED`. Invalid →
  `SecurityException("Invalid authenticator configuration")`. androidx relies on that
  exception to detect that identity check is unavailable.
- `canAuthenticate` = `PreAuthInfo.getCanAuthenticateResult()`, where
  `credentialAvailable = ITrustManager.isDeviceSecure(userId, deviceId)` and the
  biometric sensors are the registered ones. With **no sensors**:

  | authenticators | device not secure | device secure |
  | --- | --- | --- |
  | `BIOMETRIC_WEAK` or `BIOMETRIC_STRONG` | `BIOMETRIC_ERROR_NO_HARDWARE` (12) | 12 |
  | `DEVICE_CREDENTIAL` | `BIOMETRIC_ERROR_NONE_ENROLLED` (11) | `BIOMETRIC_SUCCESS` (0) |
  | biometric \| `DEVICE_CREDENTIAL` | 11 | 0 |
  | `IDENTITY_CHECK` (with the permission; never active here) | `BIOMETRIC_ERROR_IDENTITY_CHECK_NOT_ACTIVE` (20) | 20 |
  | `0` | 12 ("No authenticators requested") | 12 |

  (internal status → `BiometricConstants` → `BiometricManager`: `BIOMETRIC_NO_HARDWARE`
  → `HW_NOT_PRESENT` 12 → `NO_HARDWARE` 12; `CREDENTIAL_NOT_ENROLLED` →
  `NO_DEVICE_CREDENTIAL` 14 → `NONE_ENROLLED` 11.) The deprecated `canAuthenticate()`
  asks `BIOMETRIC_WEAK`.
- `authenticate(token, operationId, userId, receiver, opPackageName, promptInfo)`:
  app-op `OP_USE_BIOMETRIC` denied, any null argument, or a caller that is not in the
  foreground (`Utils.isForeground`) → `receiver.onError(TYPE_NONE,
  BIOMETRIC_ERROR_CANCELED, 0)` and `-1`, without an exception. PromptInfo options that
  need `TEST_BIOMETRIC`/`USE_BIOMETRIC_INTERNAL`/`SET_BIOMETRIC_DIALOG_ADVANCED` are
  enforced. `authenticators == 0` becomes `DEVICE_CREDENTIAL | BIOMETRIC_WEAK` when
  `isDeviceCredentialAllowed()`, otherwise `BIOMETRIC_WEAK`
  (`combineAuthenticatorBundles`). The request id is returned at once; the work runs on
  BiometricService's handler. A request that cannot start gets
  `onError(modality, error, 0)` from `getPreAuthenticateStatus()` (no sensors:
  `TYPE_NONE`/12 for biometric-only, `TYPE_CREDENTIAL`/14 when the credential was asked
  for but the device is not secure). When a credential is asked for and available but
  no sensor is eligible, the prompt is reduced to `DEVICE_CREDENTIAL`. A new
  `authenticate` force-cancels the current session (its client gets
  `BIOMETRIC_ERROR_CANCELED`).
- Outcomes (`AuthSession.onDialogDismissed`): credential confirmed → the credential's
  attestation is added to keystore (`KeyStoreAuthorization.addAuthToken`) and
  `onAuthenticationSucceeded(AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL = 1)`;
  biometric confirmed → `..._BIOMETRIC = 2`; user cancel →
  `BIOMETRIC_ERROR_USER_CANCELED` (10); negative button → `onDialogDismissed(NEGATIVE)`
  (the client runs the button's listener); errors → their codes, e.g.
  `BIOMETRIC_ERROR_LOCKOUT` (7), `BIOMETRIC_ERROR_HW_UNAVAILABLE` (1).
  `cancelAuthentication(token, opPackageName, requestId)` cancels the current session
  only (`getAuthSessionIfCurrent`) and the client gets `BIOMETRIC_ERROR_CANCELED` (5).
  The session links to the receiver's death and cancels without a callback
  (`AuthSession.binderDied` → `onClientDied`). SystemUI closes the prompt with
  `DISMISSED_REASON_USER_CANCEL` when its owner leaves the foreground
  (`AuthController.cancelIfOwnerIsNotInForeground`).
- Client side (BiometricPrompt.java): callbacks run on the app's executor; a second
  `authenticate` while the prompt shows gets `BIOMETRIC_ERROR_CANCELED` locally;
  `authenticate(CryptoObject, ...)` refuses non-Strong biometric bits
  (`IllegalArgumentException("Only Strong biometrics supported with crypto")`) but
  allows `DEVICE_CREDENTIAL`; `CryptoObject.getOpId()` calls
  `AndroidKeyStoreProvider.getKeyStoreOperationHandle`, which throws
  `IllegalArgumentException` for a primitive not backed by keystore2's provider.
- `BiometricManager.Strings` (`getButtonLabel`/`getPromptMessage`/`getSettingName`):
  with no sensors the modality is `TYPE_CREDENTIAL` exactly when `DEVICE_CREDENTIAL` is
  requested, so the answers are `screen_lock_app_setting_name`,
  `screen_lock_dialog_default_subtitle` and `screen_lock_app_setting_name`, or null.
- `getLastAuthenticationTime`: no gatekeeper secure user id → `BIOMETRIC_NO_AUTHENTICATION`
  (-1). The client throws `IllegalArgumentException` for authenticators other than
  `DEVICE_CREDENTIAL`/`BIOMETRIC_STRONG`.

### `KeyguardManager` (KeyguardManager.java)

`isDeviceSecure()` = `ITrustManager.isDeviceSecure(userId, deviceId)`;
`isKeyguardSecure()` = `IWindowManager.isKeyguardSecure(userId)`;
`createConfirmDeviceCredentialIntent()` returns null unless `isDeviceSecure()`, else an
`ACTION_CONFIRM_DEVICE_CREDENTIAL` intent for the system package that resolves it
(Settings' ConfirmDeviceCredentialActivity, which runs a `DEVICE_CREDENTIAL`
BiometricPrompt and finishes with `RESULT_OK`/`RESULT_CANCELED`). BiometricService uses
the same `isDeviceSecure` as `credentialAvailable`, so the two must never disagree.

### Strength classes and keystore

`BIOMETRIC_STRONG` (Class 3) is defined by keystore integration: a successful Strong
biometric adds a HardwareAuthToken that releases auth-bound keys and crypto operations
(`operationId`). `BIOMETRIC_WEAK` (Class 2) needs a real biometric match but no key
binding. `DEVICE_CREDENTIAL` is the PIN/pattern/password; confirming it also adds an
auth token (gatekeeper's attestation). `FingerprintManager` exists only with
`FEATURE_FINGERPRINT` (`getServiceOrThrow` for targetSdk ≥ 26).

## Windows APIs

Verified from Microsoft's documentation sources (GitHub `MicrosoftDocs/winrt-api` and
`MicrosoftDocs/sdk-api`; learn.microsoft.com itself was not reachable from the
authoring environment):

- `Windows.Security.Credentials.UI.UserConsentVerifier.CheckAvailabilityAsync()` →
  `UserConsentVerifierAvailability`: Available 0, DeviceNotPresent 1,
  NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4.
- Desktop apps verify through `IUserConsentVerifierInterop::RequestVerificationForWindowAsync(
  HWND appWindow, HSTRING message, REFIID riid, void** asyncOperation)`
  (`userconsentverifierinterop.h`, IID `39E050C3-4E74-441A-8DC0-B81104DF949C`; the docs
  page lists build 22000 as the minimum, the SDK header guards it with
  `NTDDI_WIN10_RS3`),
  an `IAsyncOperation<UserConsentVerificationResult>`: Verified 0, DeviceNotPresent 1,
  NotConfiguredForUser 2, DisabledByPolicy 3, DeviceBusy 4, RetriesExhausted 5
  ("after 10 attempts"), Canceled 6.
- The result says only that the signed-in user verified with "a Microsoft Passport PIN,
  Windows Hello biometric, or fingerprint reader"; **it does not say which**, and it
  binds no key.
- `Windows.Security.Credentials.KeyCredentialManager` (IsSupportedAsync,
  RequestCreateAsync, OpenAsync; `KeyCredential.RequestSignAsync`) holds Windows Hello
  keys: RSA-2048, PKCS#1 PSS/SHA-256 signatures, with attestation;
  `KeyCredentialStatus`: Success 0, UnknownError 1, NotFound 2, UserCanceled 3,
  UserPrefersPassword 4, CredentialAlreadyExists 5, SecurityDeviceLocked 6. Each use
  prompts the user; the key type is fixed.
- Not verified here (stated for maintainers to check): the NCrypt "Microsoft Passport
  Key Storage Provider" key properties, whether WinBio can attribute a Hello
  verification to a biometric, `RequestVerificationForWindowAsync` on Windows 10, from an
  MTA thread, and with a window of another process as owner.

## Decision

1. **Register `auth` always, as AOSP does** (`com.gesher.server.AuthService`,
   `IAuthService` with a StubGen stub). With no sensors and the current `trust`, it
   answers exactly what a sensor-less, unsecured AOSP device answers (table above):
   biometrics `NO_HARDWARE`, credential `NONE_ENROLLED`, the same permission,
   validation, fast-fail, cancel and death contracts. This replaces today's null
   manager and in-app NullPointerException with Android-defined unavailable results.
   It claims nothing new.
2. **No biometric authenticator is registered.** Windows Hello verification is never
   reported as `BIOMETRIC_WEAK` or `BIOMETRIC_STRONG`, `AUTHENTICATION_RESULT_TYPE_BIOMETRIC`
   is never sent, and no `FEATURE_FINGERPRINT`/`FEATURE_FACE`/`fingerprint`/`face`
   service is added: UserConsentVerifier cannot tell a biometric from the PIN, and a
   Strong claim needs key binding Gesher does not have.
3. **Windows Hello verification is a device-credential confirmation only** (route a).
   Windows Hello always includes a PIN, and a successful verification proves the
   signed-in user confirmed a credential of this account — the meaning of
   `DEVICE_CREDENTIAL`. The service talks to it through `WindowsHelloBackend`
   (availability, `verify(request, completion)` with cancel); its native implementation
   is specified in `docs/research/windows-hello-native-spec.md` and not part of phase 1.
   There is no production fake: Services passes no backend until the natives exist.
4. **`credentialAvailable` comes from `trust`, as in AOSP**, so `canAuthenticate`,
   BiometricPrompt and `KeyguardManager.isDeviceSecure()` cannot disagree. Phase 1 does
   not change TrustService (still false), hence no production behavior where a
   credential is offered. Phase 2 makes TrustService and the auth service read the same
   backend answer.
5. **A crypto-bound request (`operationId != 0`) is not confirmed** while Gesher's
   keystore cannot receive an auth token: it ends with
   `BIOMETRIC_ERROR_HW_UNAVAILABLE`, never success. (With Gesher's software keystore
   `CryptoObject.getOpId()` already throws in the app, so this is defensive.)
6. **Outcome mapping** for a credential confirmation:

   | Windows result | Android callback |
   | --- | --- |
   | Verified | `onAuthenticationSucceeded(AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL)` |
   | Canceled, after the app's `cancelAuthentication` | `onError(TYPE_CREDENTIAL, BIOMETRIC_ERROR_CANCELED)` (sent at cancel time) |
   | Canceled by the user | `onError(TYPE_CREDENTIAL, BIOMETRIC_ERROR_USER_CANCELED)` |
   | RetriesExhausted | `BIOMETRIC_ERROR_LOCKOUT` |
   | NotConfiguredForUser | `BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL` |
   | DeviceNotPresent, DisabledByPolicy, DeviceBusy, an HRESULT failure | `BIOMETRIC_ERROR_HW_UNAVAILABLE` |

   Exactly one terminal callback per request; late backend results after cancel,
   replacement or the client's death are dropped.

## Rejected alternatives

- **Report Windows Hello as `BIOMETRIC_STRONG`** (what banking apps ask for): a Strong
  authenticator must unlock auth-bound keys and crypto operations; Gesher's keystore
  neither binds keys to the user nor accepts tokens, and UserConsentVerifier does not
  say a biometric was used. Fabricated capability — outside scope (CONTRIBUTING).
- **Report it as `BIOMETRIC_WEAK`**: a PIN entry would be reported as a biometric match
  (`AUTHENTICATION_RESULT_TYPE_BIOMETRIC`). Only acceptable if a backend can prove the
  match was biometric (route c, open question 2).
- **Keep `auth` missing**: apps crash (BiometricPrompt NPE) or see a null manager that
  no AOSP device has.
- **Answer `isDeviceSecure=true` whenever the Windows account has any password** without
  a way to confirm that credential: the confirm-credential flow would then always fail.
  Phase 2 ties "secure" to a credential Gesher can verify (open question 3).
- **Unlock keys on any Windows Hello success inside the software keystore** (route b
  done halfway): keys would still be plain files usable without authentication; the
  enforcement would be cosmetic.

## Phases

1. *(this change, Java only)* `auth` stand-in with AOSP's sensor-less answers, the
   backend interface, the BiometricPrompt session plumbing (callbacks, cancellation,
   replacement, client death), host tests. No production capability change except
   "null manager / NPE" → AOSP's unavailable results.
2. Native `WindowsHelloBackend` (UserConsentVerifier, spec linked above); TrustService
   `isDeviceSecure` and `IWindowManager.isKeyguardSecure` from the backend's availability
   (open question 3); `FEATURE_SECURE_LOCK_SCREEN` with it; an
   `ACTION_CONFIRM_DEVICE_CREDENTIAL` activity that runs the same verification and
   returns `RESULT_OK`/`RESULT_CANCELED`; cancel with `USER_CANCELED` when the owner
   leaves the foreground; Windows' foreground-window rules.
3. Keystore: a real key-bound path (route b) — keys wrapped by a Windows Hello /
   TPM-protected key (KeyCredentialManager or the Passport KSP), auth tokens recorded
   on confirmation, `setUserAuthenticationRequired` enforced, `getLastAuthenticationTime`
   answered, `operationId` honoured. Only after that could a Strong claim be discussed.
4. Biometric classes only if open question 2 finds a truthful signal.

## Open questions for maintainers

1. Is `DEVICE_CREDENTIAL`-only the accepted scope for Windows Hello, knowing many
   banking apps require `BIOMETRIC_STRONG` and will still refuse?
2. Is there an acceptable signal that a Hello verification used a biometric (WinBio
   event/session attribution, Hello "sign-in options" policy that disables the PIN), so
   `BIOMETRIC_WEAK` could ever be truthful?
3. `isDeviceSecure` semantics: "Windows Hello is configured for the user" (verifiable,
   but under-reports password-only accounts) or "the account has any sign-in
   credential" (needs a second verifier, e.g. CredUI + LogonUser, for the confirm flow)?
4. Keystore enforcement: should phase 3 wrap the existing software keystore with a
   Hello/TPM key, or should auth-bound key generation fail until it exists
   (`InvalidAlgorithmParameterException`/`UserNotAuthenticatedException` as a device
   without a secure lock screen would)? Today such keys are created and usable without
   authentication.
5. Lockout: is `BIOMETRIC_ERROR_LOCKOUT` the right code for Windows' RetriesExhausted on
   a credential prompt (AOSP's credential UI handles its own throttling)?

## Consequences

- Phase 1: apps get `BiometricManager`, `canAuthenticate` answers like a phone without a
  sensor or screen lock, BiometricPrompt fails with `HW_NOT_PRESENT`/
  `NO_DEVICE_CREDENTIAL` instead of crashing. Apps that need a biometric still refuse,
  truthfully.
- Verified only by host tests compiling the unchanged service against stand-ins; the
  StubGen stub, ART dispatch, real binder death and any Windows behavior need a Windows
  build.
