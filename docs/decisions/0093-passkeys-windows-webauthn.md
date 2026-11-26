# 0093: Passkeys through Windows' WebAuthn API

Status: proposed, 2026-11-24 (see docs/passkeys.md)

## Context

Android applications create and use passkeys through `CredentialManager`: the
framework's `android.credentials` API, and `androidx.credentials`, which calls the
framework service from API 34. AOSP's `CredentialManagerService` owns no
credentials. It binds the `CredentialProviderService` of every enabled provider
application, collects their entries, shows them in a selector
(`com.android.credentialmanager`) and returns the chosen provider's answer. AOSP
ships no passkey provider; on a phone it is Google Password Manager or a password
manager the user installed.

Gesher registered no `credential` service, so `getSystemService` returned null and
`androidx.credentials` answered that the device does not support Credential Manager.

Windows has what a provider needs: `webauthn.dll` creates and uses WebAuthn
credentials in Windows Hello, on security keys and on phones, with its own dialog,
and returns the authenticator's signed data.

## Decision

1. **The system server is the device's one credential provider.**
   `CredentialManagerService` implements `ICredentialManager` and serves public-key
   requests itself over `PasskeyBackend` (`WindowsWebAuthn`). Windows' dialog is the
   selector. What an application observes is the Binder contract and the response
   and error types its library parses; which process hosts the provider is not
   observable.
2. **Without Windows' WebAuthn API, and for every other credential type, the answers
   are AOSP's for a device without a provider**: `TYPE_NO_CREATE_OPTIONS`,
   `TYPE_NO_CREDENTIAL`. The service is registered on every Windows, as the
   framework expects a device with the API to have it.
3. **Gesher builds `clientDataJSON`, with the origin Android providers use for an
   application**: `android:apk-key-hash:` and the base64url SHA-256 of its signing
   certificate, taken from the verified APK signature. A caller-set origin is for
   browsers that a provider trusts through an allowlist. Gesher has no allowlist,
   so after AOSP's permission check such a request is declined, and the application
   sees that no provider took it.
4. **The relying party must have delegated to the application.** Before any
   ceremony, `https://<rpId>/.well-known/assetlinks.json` must grant
   `delegate_permission/common.get_login_creds` to the package and one of its
   signing certificates. Any doubt (no file, a redirect, a wrong content type,
   malformed JSON, another package or certificate, an id that is not a plain host
   name) is DOM `SecurityError`. Results are not cached: each ceremony checks.
5. **Results are Windows' results.** A response is serialized only from bytes
   Windows returned for `S_OK`. User cancellation, a missing credential and a
   missing authenticator map to the framework's types; other failures keep the DOM
   error name Windows gives the HRESULT. A request the application canceled gets no
   callback, as in AOSP.
6. **The dialog is owned by the calling application's foreground window**, the rule
   of the Windows Hello bridge. A caller without the foreground gets
   `TYPE_UNKNOWN`, the framework's answer when the selector cannot be started.
7. **`clearCredentialState` succeeds when the provider is present.** It asks
   providers to forget sign-in state they keep for the application. This provider
   keeps none, so there is nothing left to clear. It does not delete credentials,
   on Android or here.
8. **The request JSON is read by a strict reader of Gesher's own** (`PasskeyJson`)
   rather than `org.json`, which accepts comments, unquoted strings and duplicate
   members. The input comes from applications and from the network.
9. **`webauthn.dll`'s structures are declared by their ABI** in `webauthn_win.cc`,
   from Microsoft's MIT-licensed `webauthn.h`, because llvm-mingw's header stops at
   API version 3 and the Windows SDK's depends on its release. Field layouts were
   checked against that header with `static_assert` for x86_64, i686 and aarch64.

## Alternatives

- **A provider application** (an APK with a `CredentialProviderService`) plus AOSP's
  provider sessions and selector. It reproduces AOSP's internals, needs a second
  selection step before Windows' own dialog, and gives applications nothing they
  can observe. It becomes worthwhile if third-party providers are to be hosted;
  the service's Binder surface would not change.
- **microG's FIDO implementation** through Play Services' `Fido2ApiClient`. It serves
  a different API, and would keep credentials in an Android software or USB
  authenticator instead of the user's Windows Hello.
- **Skipping the asset-links check**, since Windows shows the relying party id in its
  dialog. Then any application could obtain assertions for any site whose passkey
  the user has, with the user's confirmation as the only barrier. Android's
  providers do not allow that.

## Consequences

- Passkeys an application creates are Windows credentials: they appear in Windows'
  passkey settings, and sync or not as Windows decides.
- The application's origin differs from a web page's origin for the same relying
  party, as on Android. Relying parties that accept Android applications already
  accept `android:apk-key-hash:` origins.
- `prepareGetCredential`, conditional (autofill) requests, passwords, federated
  credentials and third-party providers are not served; each has AOSP's answer for
  a device without them (docs/passkeys.md, "Limits").
- Each ceremony makes one HTTPS request to the relying party before the dialog.

## Open question

`PackageManager.FEATURE_CREDENTIALS` (`android.software.credentials`) is not
declared by this change. AOSP starts its service only on devices with the feature.
Declaring it is a capability applications can see, so it is left to the maintainers:
it would be truthful only together with this service and a Windows that has
`webauthn.dll`.
