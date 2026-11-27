# 0091: Biometric and device-credential authentication over Windows Hello (auth track)

Status: Device-credential verification approved by the maintainer on 2026-11-26.
Windows Hello supplies Android's device credential where Windows exposes a usable verifier
for the desktop user's ordinary runtime. The protected runtime runs under a private service
identity, so all credential features and requests stay unavailable there.
Biometric-only and keystore-bound verification remain unavailable. Authenticating the
Windows user does not authorize access to AndroidKeyStore keys.

The software AndroidKeyStore rejects new keys that require user authentication. GKS1 did
not record whether a secret or private key needed authentication. Before starting an app,
the host preserves the original legacy records in a Windows-protected archive and removes
the old key encodings from the store the app can read. The store preserves aliases and
certificates and marks the old keys unrecoverable. The provider refuses raw legacy stores
until the host has migrated them. Trusted certificates and new keys that do not require
authentication remain accessible. Concurrent processes serialize store updates before
replacing the file. Failed writes leave the previous file and in-memory aliases in place.

The archive is outside the app's sandbox. Its Windows ACL excludes isolated applications.
An application running with the host's full Windows token has that user's filesystem access.
Gesher cannot revoke copies of key material already obtained by an application, and the
archive provides no encryption or hardware-backed protection. In the private USB
runtime, only the enrolled system server can request archival; the SYSTEM authority
validates the fixed package data roots and performs the migration. Applications
cannot use the request to select an arbitrary source or archive directory. No raw
key bytes are returned over JNI or the service protocol.

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
  verification to a biometric, and `RequestVerificationForWindowAsync` on Windows 10.
- Measured since, on Windows 11 Pro build 26300 with `tests/biometrics/hello_probe.cc`
  (the production `hello_win.cc` functions): the call works from an MTA thread with the
  process's own window as owner (`verify` → Verified, `cancel` → Canceled), and with
  another process's foreground window as owner (decision 11).

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
   *(2b, implemented: section "Phase 2b" below. `IWindowManager.isKeyguardSecure`,
   the feature, the confirm-credential intent and the foreground rule are decided there.)*
3. Keystore: a real key-bound path (route b) — keys wrapped by a Windows Hello /
   TPM-protected key (KeyCredentialManager or the Passport KSP), auth tokens recorded
   on confirmation, `setUserAuthenticationRequired` enforced, `getLastAuthenticationTime`
   answered, `operationId` honoured. Only after that could a Strong claim be discussed.
4. Biometric classes only if open question 2 finds a truthful signal.

## Phase 2b: the credential reaches apps

Phase 2 left `trust` answering "not secure", so no production path reached Windows Hello.
Phase 2b turns it on. Sources as above (`android-16.0.0_r4` GitHub mirrors;
`MicrosoftDocs/sdk-api`, `MicrosoftDocs/winrt-api`).

### Contracts added to the picture

- `KeyguardManager.isDeviceSecure()` is `mTrustManager.isDeviceSecure(userId, deviceId)`;
  `isKeyguardSecure()` is `mWM.isKeyguardSecure(mContext.getUserId())`
  (KeyguardManager.java). `TrustManagerService.isDeviceSecure` answers false for
  `deviceId != Context.DEVICE_ID_DEFAULT` ("Virtual devices are considered insecure"),
  passes the user through `ActivityManager.handleIncomingUser(..., requireFull)` and
  returns `mLockPatternUtils.isSecure(userId)`. `WindowManagerService.isKeyguardSecure`
  throws `SecurityException("Requires INTERACT_ACROSS_USERS permission")` for another
  user without it and returns `mPolicy.isKeyguardSecure(userId)`
  (PhoneWindowManager → the keyguard: a secure lock screen or a locked SIM).
- `PreAuthInfo.create`: `credentialAvailable = trustManager.isDeviceSecure(userId,
  context.getDeviceId())`. `getInternalStatus`: biometric and credential requested, no
  sensor: `credentialAvailable` → `TYPE_CREDENTIAL`/`AUTHENTICATOR_OK`, else
  `CREDENTIAL_NOT_ENROLLED`; biometric only, no sensor: `BIOMETRIC_NO_HARDWARE`;
  credential only: `credentialAvailable ? AUTHENTICATOR_OK : CREDENTIAL_NOT_ENROLLED`.
  AuthService already computed exactly this (phase 1's table); only its input changes.
- `FEATURE_SECURE_LOCK_SCREEN`: "The device has a secure implementation of keyguard,
  meaning the device supports PIN, pattern and password as defined in Android CDD"
  (PackageManager.java). It is a property of the device, read once:
  `LockSettingsService.systemReady` stores `mHasSecureLockScreen =
  hasSystemFeature(FEATURE_SECURE_LOCK_SCREEN)` and without it refuses to set any
  credential (`UnsupportedOperationException("This operation requires secure lock screen
  feature")`), so a device without the feature is never secure.
- `createConfirmDeviceCredentialIntent`: `if (!isDeviceSecure()) return null;` then
  `ACTION_CONFIRM_DEVICE_CREDENTIAL` with `EXTRA_TITLE`/`EXTRA_DESCRIPTION` and
  `setPackage(getSettingsPackageForIntent(intent))`, which is the first
  `MATCH_SYSTEM_ONLY` activity's package or, with none, `"com.android.settings"`.
- Settings' `ConfirmDeviceCredentialActivity` (packages/apps/Settings,
  `password/ConfirmDeviceCredentialActivity.java`; exported, filter for
  `CONFIRM_DEVICE_CREDENTIAL`, `CONFIRM_FRP_CREDENTIAL` and the repair-mode actions):
  title from `getStringExtra(EXTRA_TITLE)`, details from
  `getCharSequenceExtra(EXTRA_DESCRIPTION)`, authenticators from the int extra
  `biometric_prompt_authenticators`, default `DEVICE_CREDENTIAL | BIOMETRIC_WEAK`; the
  real caller is `getCallingActivity()`. It shows a BiometricPrompt and keeps itself
  alive "until BiometricPrompt goes away". `onAuthenticationSucceeded` →
  `setResult(Activity.RESULT_OK, mIntentData); finish()`. `onAuthenticationError` with
  `USER_CANCELED` or `CANCELED` → `finish()` (the result stays `RESULT_CANCELED`); other
  errors, when `DEVICE_CREDENTIAL` was requested, → `showConfirmCredentials()` (the
  PIN/pattern/password screen), or, when it was not, "Finishing, device credential not
  requested". `showConfirmCredentials` on a device with no credential:
  `Log.d(TAG, "No pin/pattern/pass set"); setResult(Activity.RESULT_OK)`.
- `createConfirmFactoryResetCredentialIntent` throws `UnsupportedOperationException`
  without `LockPatternUtils.frpCredentialEnabled` or a persistent data block service, and
  `IllegalStateException` on a provisioned device.
- Owner in the background: `AuthController.onTaskStackChanged` → `isOwnerInBackground()`
  → `cancelIfOwnerIsNotInForeground()` → `closeDialog(DISMISSED_REASON_USER_CANCEL, ...)`
  → `mReceiver.onDialogDismissed(reason)`, which AuthSession reports to the client as
  `BIOMETRIC_ERROR_USER_CANCELED`.
- Windows: `RequestVerificationForWindowAsync`: "req.target-min-winverclnt: Windows Build
  22000"; `appWindow`: "Handle to the window of the active application."
  `UserConsentVerifierAvailability`: Available "An authentication device is available.",
  DeviceNotPresent "There is no authentication device available.", NotConfiguredForUser
  "An authentication device is not configured for this user.", DisabledByPolicy "Group
  policy has disabled the authentication device.", DeviceBusy "The authentication device
  is performing an operation and is unavailable."

### Decisions

7. **One availability decision, `DeviceCredential`.** The credential is available when
   the Windows Hello natives are in the runtime, Windows' build (`RtlGetVersion`) is
   22000 or later, the device supports a secure lock screen (decision 8) and
   `CheckAvailabilityAsync` answers `Available` now (cached for 5 s), or answers
   `DeviceBusy` or fails after an `Available` within the last 10 minutes (see below). `trust`'s
   `isDeviceSecure`, the window manager's `isKeyguardSecure`, `auth` (through `trust`, as
   PreAuthInfo, and directly before a prompt) and the confirm-credential activity read
   this one object. `isDeviceLocked` and `isKeyguardLocked` stay false: an app runs in
   the user's unlocked Windows session. Below build 22000 the device is simply not
   secure, whatever Windows Hello's state.
   **A busy Windows Hello is not "no credential"** (review of #470). Android keeps two
   questions apart: does the user have a credential (`PreAuthInfo`'s `credentialAvailable`
   from `ITrustManager.isDeviceSecure`, `CREDENTIAL_NOT_ENROLLED` ->
   `BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL` -> `BIOMETRIC_ERROR_NONE_ENROLLED`), and can the
   authenticator work now (`Utils.biometricConstantsToBiometricManager`/`authenticatorStatusToBiometricConstant`:
   every other status, `default:` after `BIOMETRIC_HARDWARE_NOT_DETECTED`, ->
   `BIOMETRIC_ERROR_HW_UNAVAILABLE`; android-16.0.0_r4 `services/core/java/com/android/server/biometrics/Utils.java`).
   Windows' `DeviceBusy` ("The authentication device is performing an operation") and a failed
   query answer only the second question. So:
   - `DeviceCredential.available()` (`isDeviceSecure`, `isKeyguardSecure`,
     `canAuthenticate(DEVICE_CREDENTIAL)`, the confirm-credential activity) keeps the last
     definite answer across a busy or failed reading: enrolled stays enrolled, not enrolled
     stays not enrolled (a busy reading never creates a credential).
   - A definite answer (`Available`, `NotConfiguredForUser`, `DisabledByPolicy`,
     `DeviceNotPresent`) replaces what is remembered at once.
   - **Trust bound: 10 minutes** (`BUSY_TRUST_MS`) from the last definite answer. A device
     that is occupied for seconds is busy; one that does not answer for 10 minutes is a
     failing Windows Hello, and Gesher can no longer say that a credential exists, so
     `isDeviceSecure` becomes false then. AOSP has no "unknown" for it. This is the one
     deliberate edge of the rule, and the bound is a constant, not a Windows fact.
   - A prompt attempted while busy ends with `BIOMETRIC_ERROR_HW_UNAVAILABLE`
     (`AuthService.unavailable`: `DeviceCredential.state()` is the live answer), with no
     Windows prompt; a confirm-credential intent ends `RESULT_CANCELED`.
   - At start, a busy or failed first answer is asked again 3 times, 250 ms apart, before
     decision 8 reads it. Each retry bypasses the ordinary five-second availability
     cache and stores Windows' fresh answer for subsequent normal reads.
8. **`FEATURE_SECURE_LOCK_SCREEN` is decided once, when the system server starts**, as
   Android fixes features at boot: declared when Windows then answers `Available` or
   `NotConfiguredForUser` (Windows has a verifier; the user has or has not set it up, as
   a phone's user has or has not set a screen lock), absent for `DeviceNotPresent`,
   `DisabledByPolicy`, `DeviceBusy`, a failed query, Windows below 22000 or no natives.
   The user's later changes move only the state, never the feature:
   - set up → removed: the feature stays, `isDeviceSecure` becomes false (a phone whose
     user removed the lock);
   - not set up → set up: `isDeviceSecure` becomes true without a restart;
   - no feature at start → Windows Hello appears later: the device stays not secure
     until Gesher's system server restarts. This keeps AOSP's invariant that a device
     without the feature is never secure (LockSettingsService above), at the cost of a
     restart; a `DeviceBusy` or failed query that lasts through the start-up retries (decision 7)
     has the same cost.
9. **`ACTION_CONFIRM_DEVICE_CREDENTIAL` is answered by the activity manager**
   (`ConfirmCredential`, `ActivityTaskManagerService.confirmDeviceCredential`): Gesher has
   no Settings package, as for the Bluetooth consent activities. It matches the action
   with no package or `com.android.settings`, no component, data or type. The calling
   activity is covered and paused like under any system activity, the same `AuthService`
   session as a BiometricPrompt runs (title and description as Windows' message,
   `DEVICE_CREDENTIAL | BIOMETRIC_WEAK`, or the intent's authenticators), and the result
   is `RESULT_OK` **only** for Windows' `Verified`. Windows' prompt is owned by the
   calling process's own foreground window, never another app's. The caller finishing or
   its process dying cancels the verification.
   - *Deviation, deliberate*: where Settings answers `RESULT_OK` because no credential
     is set at all, Gesher answers `RESULT_CANCELED`. Gesher cannot state that a Windows
     account has no credential (it may have a password Windows Hello does not cover),
     and an `OK` there would report a confirmation nobody gave.
   - Windows' `RetriesExhausted` is `RESULT_CANCELED` (Settings would fall back to its
     own credential screen, which Gesher does not have).
   - Settings' private `extra_data` result intent is not returned; the result has no data.
   - `CONFIRM_FRP_CREDENTIAL`, the repair-mode, remote-device and `_WITH_USER` actions
     are not handled and stay unresolved (`ActivityNotFoundException`): Gesher has no
     factory reset protection, repair mode or second user.
   - Package manager queries do not list a Settings activity for the action, so
     `getSettingsPackageForIntent` falls to `"com.android.settings"`, which is what the
     activity manager matches.
10. **BiometricPrompt with `DEVICE_CREDENTIAL`** (alone, or with biometric bits, which
    fall to the credential since there is no sensor) now reaches Windows Hello. A
    biometric-only request stays `BIOMETRIC_ERROR_HW_NOT_PRESENT`; a request with a
    `CryptoObject` (`operationId != 0`) stays `BIOMETRIC_ERROR_HW_UNAVAILABLE` until the
    keystore enforces authentication (phase 3).
11. **The prompt is started in the system server with the app process's window as
    owner** (`RequestVerificationForWindowAsync(appWindow = the root owner of the
    foreground window, when it is the app's)`). Windows accepts it: measured on
    Windows 11 Pro build 26300 with `hello_probe host` as the "app" and a second
    process asking, `foreign-verify` ended `Verified` (the user entered the PIN, 19.5 s)
    and `foreign-cancel` ended `Canceled` 3.1 s after the start (the "app's" cancel at
    3 s). That is one machine and one build, and the probe, not the system server
    across Binder. *Fallback, kept as a note only and not implemented*: should a
    Windows build refuse a window of another process (`E_INVALIDARG`/`E_ACCESSDENIED`,
    reported today as `BIOMETRIC_ERROR_HW_UNAVAILABLE`), the call moves into the app
    process: a Gesher-internal `StandIn.Extension` transaction on the app's
    `IApplicationThread` binder runs `Start`/`Await`/`Cancel` there against its own
    window and returns Windows' result; the session, its rules and the mapping stay in
    the system server, which must then treat the app as untrusted for the result (an
    app could answer "Verified" itself). That weakness is why the direct path is the
    one implemented.
12. **Owner leaving the foreground, and app death.** While Windows' prompt shows,
    `WindowsHello` samples every 250 ms where Windows' foreground window is relative to
    the owner (`hello_owner.cc`, decided by `OwnerWatch`): a window of the app's process
    (*owner*), a window of another process whose root owner is the app's window
    (*owned*: Windows' prompt), anything else (*other*), or the owner window no longer
    existing (*gone*). Two consecutive *other* samples after the prompt was seen, or
    *gone*, cancel the verification; Windows' `Canceled` is reported as
    `BIOMETRIC_ERROR_USER_CANCELED`, as SystemUI's dismissal. The client's Binder death
    still ends the session without a callback (phase 1), and the confirm-credential
    request is canceled when its caller's activity or process is gone.
    Microsoft documents `appWindow` only as "the window of the active application", not
    that the prompt is a window owned by it. So the watch cancels for *other* only
    after it has seen an *owned* window once: if a Windows build hosts the prompt
    without that ownership, no prompt is canceled for a foreground change (it stays
    until the user answers it; the log says so once), instead of every prompt being
    canceled the moment it appears. `hello_owner_probe` shows which it is on a machine.

### Still open after 2b

- The `RESULT_OK`-without-a-credential deviation in decision 9, and whether the
  confirm-credential intent should instead stay unresolved while the device is not
  secure.
- Whether the boot-time decision for the feature (decision 8) is preferred over
  declaring it whenever Windows is 22000 or later.

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
