# Passkeys: Credential Manager over Windows' WebAuthn API

Android applications sign in with passkeys through `CredentialManager`
(`android.credentials`, and `androidx.credentials`, which uses the framework service
from API 34). Gesher serves that API with Windows' own WebAuthn API (`webauthn.dll`):
the credential lives in Windows Hello, on a security key or on a phone, whichever the
user picks in Windows' dialog. The decision and its alternatives are in
[ADR 0093](decisions/0093-passkeys-windows-webauthn.md).

```
app ──Binder──> credential (ICredentialManager)
                 CredentialManagerService          Android's contract, permissions, sessions
                  ├─ PasskeyRequests / PasskeyJson  WebAuthn JSON in, clientDataJSON, JSON out
                  ├─ AssetLinks                     https://<rpId>/.well-known/assetlinks.json
                  └─ PasskeyBackend
                       └─ WindowsWebAuthn (JNI, null without its natives)
                            └─ compat/devices/webauthn_jni.cc → webauthn_win.cc → webauthn.dll
```

## The Android contract

AOSP's `CredentialManagerService`
([android-16.0.0_r4](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/services/credentials/java/com/android/server/credentials/CredentialManagerService.java))
hands a request to the device's credential providers and shows their entries in a
selector. AOSP ships no passkey provider. In Gesher the system server is the one
provider, and Windows' WebAuthn dialog is the selector.

| Call | With Windows' WebAuthn API | Without it (no natives, no `webauthn.dll`) |
| --- | --- | --- |
| `executeCreateCredential`, type `androidx.credentials.TYPE_PUBLIC_KEY_CREDENTIAL` | one registration ceremony; `onResponse` with `androidx.credentials.BUNDLE_KEY_REGISTRATION_RESPONSE_JSON` | `onError(TYPE_NO_CREATE_OPTIONS, "No create options available.")`, AOSP's answer when no provider takes the request ([L834](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/services/credentials/java/com/android/server/credentials/CredentialManagerService.java#L834)) |
| `executeGetCredential` with a public-key option | one assertion ceremony; `onResponse` with a `Credential` of that type holding `androidx.credentials.BUNDLE_KEY_AUTHENTICATION_RESPONSE_JSON` | `onError(TYPE_NO_CREDENTIAL, "No credentials available on this device.")` ([L646](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/services/credentials/java/com/android/server/credentials/CredentialManagerService.java#L646)) |
| Any other credential type (passwords, federated) | the same no-provider answers | the same |
| `executePrepareGetCredential` | AOSP's response for a request without results: no result types, no `PendingIntent`. The application's later `getCredential(handle)` reports `TYPE_NO_CREDENTIAL` itself | the same |
| `clearCredentialState` | `onSuccess`: this provider keeps nothing between ceremonies | `onError("UNKNOWN", ...)`, AOSP's answer without a provider |
| `getCandidateCredentials` | `SecurityException`: no credential autofill service | the same |
| `isEnabledCredentialProviderService`, `getCredentialProviderServices` | false, an empty list: third-party provider services are not hosted | the same |
| `setEnabledProviders` | `permission_denied`, or `failed_setting_store` for a caller that may write secure settings | the same |
| `registerCredentialDescription`, `unregisterCredentialDescription` | `UnsupportedOperationException("Feature not supported")`, the feature's default | the same |
| `isServiceEnabled` | true | true |

Checked on the Binder thread, as AOSP does, before anything else: the calling package
belongs to the calling uid (`SecurityException`), a request with an origin needs
`CREDENTIAL_MANAGER_SET_ORIGIN`, and one with allowed providers needs
`CREDENTIAL_MANAGER_SET_ALLOWED_PROVIDERS`.

Before this change `getSystemService(CredentialManager.class)` was null
([SystemServiceRegistry](https://github.com/msft-mirror-aosp/platform.frameworks.base/blob/android-16.0.0_r4/core/java/android/app/SystemServiceRegistry.java#L1357)
returns null without the Binder service), and `androidx.credentials` reported that
the device does not support Credential Manager.

## One ceremony

1. **Parse.** The request JSON is WebAuthn's `PublicKeyCredentialCreationOptionsJSON`
   or `PublicKeyCredentialRequestOptionsJSON`. It is read by a strict JSON reader
   (`PasskeyJson`: no comments, no duplicate members, bounded depth). `rp.id` /
   `rpId` is required and must be a plain lower-case host name.
2. **Associate.** The relying party must have delegated to the application:
   `https://<rpId>/.well-known/assetlinks.json` must grant
   `delegate_permission/common.get_login_creds` to the calling package and the SHA-256
   of one of its signing certificates, as Android's credential providers require
   ([Digital Asset Links for passkeys](https://developer.android.com/identity/sign-in/credential-manager#add-support-dal)).
   The file is fetched by the system server over HTTPS with `HttpsURLConnection`, the
   stack applications use ([network.md](network.md): Windows' proxy settings and
   Windows' trusted roots), without following redirects, as JSON, at most 1 MiB. The
   certificates come from the verified APK signature only
   (`ApkPackage.verifiedSignatures`), never from a substituted signature.
3. **Client data.** Gesher, not the application, builds `clientDataJSON`:
   `{"type":"webauthn.create"|"webauthn.get","challenge":"<base64url>","origin":"android:apk-key-hash:<base64url SHA-256 of the signing certificate>","crossOrigin":false}`,
   serialized as the WebAuthn specification prescribes. An application cannot choose
   its origin. A request that sets an origin (a browser acting for a web site) needs
   the permission above and is then declined, because Gesher has no allowlist of
   browsers: the application sees the no-provider answer.
4. **Windows.** `WebAuthNAuthenticatorMakeCredential` or
   `WebAuthNAuthenticatorGetAssertion` runs on a thread of its own with Windows'
   dialog owned by the calling process's foreground window. A caller that does not
   own the foreground window gets no dialog.
5. **Answer.** The response JSON is built from the bytes Windows returned and nothing
   else. See the tables below.

The application's `CancellationSignal` and the death of its callback cancel the
ceremony (`WebAuthNCancelCurrentOperation`); as in AOSP's canceled session, no
callback follows. Every request answers at most once. Requests of different
applications, or several of one, run concurrently.

### Request mapping

| WebAuthn JSON | Windows |
| --- | --- |
| `rp.id`, `rp.name`, `user.id`, `user.name`, `user.displayName` | `WEBAUTHN_RP_ENTITY_INFORMATION`, `WEBAUTHN_USER_ENTITY_INFORMATION` |
| `pubKeyCredParams` of type `public-key`, in order | `WEBAUTHN_COSE_CREDENTIAL_PARAMETERS`; none of that type is `NotSupportedError` |
| `challenge` | inside `clientDataJSON`, which Windows hashes with SHA-256 |
| `timeout` | `dwTimeoutMilliseconds`, kept within 10 s to 10 min; 5 min when absent |
| `excludeCredentials`, `allowCredentials` (`id`, `transports`) | `pExcludeCredentialList`, `pAllowCredentialList` |
| `authenticatorSelection.authenticatorAttachment` | `dwAuthenticatorAttachment` (any when absent) |
| `residentKey` `required` / `preferred` / `discouraged`, or `requireResidentKey` | `bRequireResidentKey`, `bPreferResidentKey` (options version 4, API version 3; before it a preference is not a requirement) |
| `userVerification` | `dwUserVerificationRequirement` (preferred when absent) |
| `attestation` `none` / `indirect` / `direct` | `dwAttestationConveyancePreference`; `enterprise` and unknown values are `none` |
| `extensions.credProps` | answered from Windows' `bResidentKey` |

Not passed on in this slice: other extensions (PRF, largeBlob, credBlob), `hints`,
enterprise attestation. They are ignored, and no result is reported for them.

### Response mapping

| Response member | Source |
| --- | --- |
| `id`, `rawId` | `pbCredentialId` / `Credential.pbId` |
| `response.clientDataJSON` | the bytes Gesher gave Windows to hash |
| `response.attestationObject`, `response.authenticatorData`, `response.signature`, `response.userHandle` | Windows' bytes, unchanged |
| `response.publicKeyAlgorithm`, `response.publicKey` | read from the credential's COSE key in the authenticator data; `publicKey` (SubjectPublicKeyInfo) is given for EC2 P-256 and RSA keys and omitted otherwise |
| `response.transports` | `dwTransports` (attestation version 8), else the one used transport, else empty |
| `authenticatorAttachment` | `platform` when the used transport is internal, `cross-platform` for another, omitted when Windows does not say |
| `clientExtensionResults.credProps.rk` | `bResidentKey` (attestation version 4); omitted when unknown |

### Results and errors

| What happened | The application gets |
| --- | --- |
| Windows returned a credential or an assertion | the response |
| The user dismissed Windows' dialog (`NTE_USER_CANCELLED`, `ERROR_CANCELLED`) | `TYPE_USER_CANCELED` (`CreateCredentialCancellationException` / `GetCredentialCancellationException`) |
| No matching credential, or no authenticator, on a get (`NTE_NOT_FOUND`, `NTE_DEVICE_NOT_FOUND`) | `TYPE_NO_CREDENTIAL` (`NoCredentialException`) |
| No authenticator on a create (`NTE_DEVICE_NOT_FOUND`) | `TYPE_NO_CREATE_OPTIONS` (`CreateCredentialNoCreateOptionException`) |
| An excluded credential exists (`NTE_EXISTS`) | DOM `InvalidStateError` |
| Any other Windows failure | the DOM error `WebAuthNGetErrorName` names (`ConstraintError`, `NotSupportedError`, `NotAllowedError`, `UnknownError`), as `androidx.credentials.TYPE_{CREATE,GET}_PUBLIC_KEY_CREDENTIAL_DOM_EXCEPTION/androidx.credentials.TYPE_<NAME>`; a time-out is Windows' `NotAllowedError` |
| The relying party did not delegate to the application, or its id is not a host name | DOM `SecurityError`, "The incoming request cannot be validated" |
| The request JSON is malformed or lacks a required member | DOM `DataError` |
| The caller does not own the foreground window | `TYPE_UNKNOWN`, as when Android cannot start its selector |
| Windows answered `S_OK` with data that cannot be read | `TYPE_UNKNOWN`; no response is made up |
| The application canceled, or died | nothing |

## Native layer

`compat/devices/webauthn_win.{h,cc}` loads `webauthn.dll` from System32 at run time
and binds its exports with `GetProcAddress`, so the runtime starts on a Windows
without it (`ApiVersion()` is then 0 and every call answers `kUnavailable`). The
structures are declared by their ABI, as
[webauthn.h](https://github.com/microsoft/webauthn/blob/master/webauthn.h) (MIT)
declares them, because the toolchains' own headers stop at different API versions.
Inputs carry the structure version the running API understands; outputs are read up
to the `dwVersion` Windows filled in.

| Windows | API version | What follows |
| --- | --- | --- |
| Server 2022 (`windows-2022` runner) | 2 | make-credential options version 3: `residentKey: "preferred"` is not expressed; attestation version 3: the used transport, no `bResidentKey`; no platform credential list |
| Server 2025 (`windows-2025` runner, 10.0.26100) | 7 | options version 4; attestation version 6: used transport and `bResidentKey` |
| API version 9 | 9 | attestation version 8: also the authenticator's transports |

The first two rows are what the runners report (`webauthn_probe info`); the versions
of each structure per API version are from webauthn.h's version table.

## Tests

| Layer | What | Where |
| --- | --- | --- |
| Java host contracts | The production service, JSON, asset links and backend interface, unchanged, against stand-ins; a software authenticator with real EC and RSA keys in place of Windows; a local HTTP server for asset links | `tools/test_credential_manager_contracts.py`, `tests/credentials/CredentialManagerContracts.java.in` |
| Windows host test, no UI | The real `webauthn.dll`: API version, platform authenticator query, cancellation ids, error names, an empty platform credential list, no ceremony without an owner window | `tests/credentials/webauthn_windows_test.cc` (ci.yml `windows-host-contracts`) |
| Manual | Real ceremonies with Windows Hello, signature verified in the probe, cross-process owner, cancellation | `tests/credentials/webauthn_probe.cc` |

The software authenticator and the local server exist only in the test harness.
Production has one backend, `WindowsWebAuthn`, and one fetcher, `AssetLinks.Https`.

## Limits

- `prepareGetCredential` finds nothing; applications that only use it for an early
  hint fall back to `getCredential`.
- Third-party `CredentialProviderService` applications (password managers) are not
  bound, so passwords and federated credentials have no provider.
- Conditional (autofill) requests are not offered: there is no credential autofill
  service.
- `PackageManager.FEATURE_CREDENTIALS` is not declared; see the ADR's open question.
- A relying party must name itself (`rp.id`), and must serve its asset links
  directly, as JSON, without redirects.
