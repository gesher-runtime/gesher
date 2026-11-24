# 0092: AndroidKeyStore user authentication enforced through a Windows Hello key

Status: proposed. Phase 3 of [0091](0091-biometric-and-device-credential-via-windows-hello.md).
Nothing here has run on Windows yet; see "Not verified".

## Context

`com.gesher.app.AndroidKeyStore` is a software key store in the app's process. Key
material is written unencrypted to `<data dir>/.gesher-keystore`, and
`KeyGenParameterSpec` authorizations are not enforced. A key made with
`setUserAuthenticationRequired(true)` is created and then works without any
authentication: the app is told something that is not true. Because of that, ADR 0091
made `AuthService` refuse every crypto-bound prompt (`operationId != 0` →
`BIOMETRIC_ERROR_HW_UNAVAILABLE`), and with plain keys `CryptoObject.getOpId()` throws in
the app anyway.

This record decides how such keys become really bound to a user confirmation, and what
an app observes when they cannot be.

## Android contracts (android-16.0.0_r4)

GitHub mirrors `msft-mirror-aosp/platform.frameworks.base` (`fb`),
`platform.system.security` (`ss`), `platform.external.conscrypt`, `platform.libcore`,
all at tag `android-16.0.0_r4`.

**What a spec asks.** `fb keystore/java/android/security/keystore/KeyGenParameterSpec.java`:
the Builder's defaults are timeout 0 and type `AUTH_BIOMETRIC_STRONG`;
`setUserAuthenticationValidityDurationSeconds(-1)` is timeout 0 with
`AUTH_BIOMETRIC_STRONG`, a value `>= 0` is that timeout with
`AUTH_DEVICE_CREDENTIAL | AUTH_BIOMETRIC_STRONG`; `setUserAuthenticationParameters(timeout,
type)` sets both. `KeyProtection` has the same three. `KeyProperties.AUTH_DEVICE_CREDENTIAL
= 1`, `AUTH_BIOMETRIC_STRONG = 2`.

**Whether such a key can be made.**
`fb keystore/java/android/security/keystore2/KeyStore2ParameterUtils.java`, lines 234-298:

```java
    private static long getRootSid() {
        long rootSid = GateKeeper.getSecureUserId();
        if (rootSid == 0) {
            throw new IllegalStateException("Secure lock screen must be enabled"
                    + " to create keys requiring user authentication");
```

```java
        if (spec.getUserAuthenticationType() == (KeyProperties.AUTH_BIOMETRIC_STRONG
                | KeyProperties.AUTH_DEVICE_CREDENTIAL)) {
            ...
                params.add(makeLong(KeymasterDefs.KM_TAG_USER_SECURE_ID, getRootSid()));
        } else {
            List<Long> sids = new ArrayList<>();
            if ((spec.getUserAuthenticationType() & KeyProperties.AUTH_BIOMETRIC_STRONG) != 0) {
                ...
                final long[] biometricSids = bm.getAuthenticatorIds();
                if (biometricSids.length == 0) {
                    throw new IllegalStateException(
                            "At least one biometric must be enrolled to create keys requiring user"
                                    + " authentication for every use");
                }
                ...
            } else if ((spec.getUserAuthenticationType() & KeyProperties.AUTH_DEVICE_CREDENTIAL)
                    != 0) {
                sids.add(getRootSid());
            } else {
                throw new IllegalStateException("Invalid or no authentication type specified.");
```

The generators run this as a check when they are initialized
(`AndroidKeyStoreKeyGeneratorSpi.java` lines 258-264, the same in
`AndroidKeyStoreKeyPairGeneratorSpi.java`):

```java
                KeyStore2ParameterUtils.addUserAuthArgs(new ArrayList<>(), spec);
            } catch (IllegalStateException | IllegalArgumentException e) {
                throw new InvalidAlgorithmParameterException(e);
```

`AndroidKeyStoreSpi.setSecretKeyEntry`/`setPrivateKeyEntry` rethrow the same exceptions
as `java.security.KeyStoreException(e)`.

**Keys with a timeout.** `ss keystore2/src/enforcements.rs`, `authorize_create`, lines
578-596: the newest auth token that satisfies the key's secure ids and type is taken,
and

```rust
            .ok_or(Error::Km(Ec::KEY_USER_NOT_AUTHENTICATED))
            ...
            if token_age.seconds() > key_time_out {
                return Err(Error::Km(Ec::KEY_USER_NOT_AUTHENTICATED)).context(ks_err!(
```

Tokens are found by secure id and type only (`AuthTokenEntry::satisfies`, `database.rs`),
so a token minted for an operation's challenge also satisfies keys with a timeout. The
Java side maps that error at operation creation, i.e. in `Cipher.init`,
`Signature.initSign`, `Mac.init`, to `UserNotAuthenticatedException` ("User not
authenticated", an `InvalidKeyException`;
`KeyStoreCryptoOperationUtils.getInvalidKeyException`).

**Keys without a timeout.** Creation of the operation succeeds and keystore2 returns an
operation challenge (`DeferredAuthState::OpAuthRequired`); the SPI returns it from
`KeyStoreCryptoOperation.getOperationHandle()`, and
`android.hardware.biometrics.CryptoObject.getOpId()` reads it through
`AndroidKeyStoreProvider.getKeyStoreOperationHandle`
(`fb keystore/java/android/security/keystore2/AndroidKeyStoreProvider.java`):

```java
        } else if (!(spi instanceof KeyStoreCryptoOperation)) {
            throw new IllegalArgumentException(
                    "Crypto primitive not backed by AndroidKeyStore provider: " + cryptoPrimitive
```

An operation that is used before a token for its challenge arrived fails with
`KEY_USER_NOT_AUTHENTICATED` in `before_update`/`before_finish`. The app sees that as
(`AndroidKeyStoreCipherSpiBase.java` lines 417-438 and 610-636,
`AndroidKeyStoreSignatureSpiBase.java`, `AndroidKeyStoreHmacSpi.java`): `Cipher.update`
returns null and remembers the error, `Cipher.doFinal` throws
`IllegalBlockSizeException` whose cause is an `android.security.KeyStoreException`
(internal code -26); `Signature.update`/`sign` throw `SignatureException(e)`;
`Mac.update`/`doFinal` throw `ProviderException("Keystore operation failed", e)`.

**BiometricPrompt with a CryptoObject.** `fb core/java/android/hardware/biometrics/BiometricPrompt.java`:
`authenticate(CryptoObject, ...)` rejects biometric bits other than Strong ("Only Strong
biometrics supported with crypto"); the `DEVICE_CREDENTIAL` bit is not looked at, so
`DEVICE_CREDENTIAL` and `BIOMETRIC_STRONG | DEVICE_CREDENTIAL` are accepted, and the
operation id travels down the credential path to gatekeeper, whose token carries it as
its challenge (`AuthSession`, SystemUI's `CredentialInteractor.verifyCredential`).

**Invalidation.** `KeyPermanentlyInvalidatedException` ("Key permanently invalidated",
an `InvalidKeyException`) is thrown at operation creation: for keystore2's
`KEY_PERMANENTLY_INVALIDATED` and `KEY_NOT_FOUND`
(`KeyStoreCryptoOperationUtils.getInvalidKeyException`), and when a key that needs an
operation challenge can no longer be authorized because its secure ids match neither the
current root secure id nor the enrolled biometrics (`getOrMakeOperationChallenge`,
`canUserAuthorizationSucceed`).

**How JCA finds the right primitive.** AndroidKeyStore keys have no format and no
encoding (`AndroidKeyStoreKey.getFormat()`/`getEncoded()` return null).
`AndroidKeyStoreBCWorkaroundProvider` registers Cipher, Mac and Signature with
`SupportedKeyClasses` of its key classes; Conscrypt registers its own with
`SupportedKeyFormats` `RAW` (Mac, symmetric Cipher), `PKCS#8|X.509` (Signature) and the
`RSAPrivateKey`/`ECPrivateKey` interfaces (`OpenSSLProvider.putMacImplClass`,
`putSymmetricCipherImplClass`, `putRSACipherImplClass`, `putSignatureImplClass`), and
libcore's `Cipher.tryCombinations`, `Signature.Delegate.chooseProvider` and
`Mac.chooseProvider` skip a service whose `supportsParameter(key)` is false. AOSP's RSA
and EC private keys implement `RSAKey`/`ECKey`, not `RSAPrivateKey`/`ECPrivateKey`.

**Precedent for sealing to a public key.** keystore2's `super_key.rs` keeps, for
`UnlockedDeviceRequired` keys, an ECDH key pair per user: new keys are encrypted to the
public key while the private key is available only when the device is unlocked. The
binding below is the same construction.

## Windows facts

From `MicrosoftDocs/winrt-api` (`windows.security.credentials/*.md`),
`MicrosoftDocs/windows-dev-docs` (`hub/apps/develop/security/windows-hello.md`),
`microsoft/win32metadata` (WinRT IDL, SDK 10.0.28000) and `microsoft/windows-rs`.

- `Windows.Security.Credentials.KeyCredentialManager`: `IsSupportedAsync`,
  `RequestCreateAsync(name, option)` ("Creates a new key credential for the current user
  and application"), `OpenAsync(name)`, `DeleteAsync(name)`;
  `KeyCredential.RetrievePublicKey()` (default blob type `X509SubjectPublicKeyInfo`) and
  `RequestSignAsync(data)` ("Prompts the user to cryptographcally sign data" [sic]).
  `KeyCredentialStatus`: Success 0, UnknownError 1, NotFound 2, UserCanceled 3,
  UserPrefersPassword 4, CredentialAlreadyExists 5, SecurityDeviceLocked 6 (the IDL also
  has AlgorithmNotSupported 7).
- The key is RSA 2048, generated in the TPM when there is a suitable one, otherwise in
  software (Windows Hello article). "An application can never use the keys from another
  application, nor can someone ever use the keys from another user." How Windows
  identifies an unpackaged desktop process as an "application" is not documented.
- After a PIN reset the keys are gone: the article's sample treats `NotFound` from
  `OpenAsync` as "PIN reset has occurred somewhere else and key is lost".
- **The signature padding is not established by the documentation.** The API reference
  (`keycredentialmanager.md`) says "Signature Format: PKCS #1 RSA PSS with SHA256"; the
  Windows Hello article says "We are using SHA256 as the hash algorithm and Pkcs1 for
  SignaturePadding" and verifies with `RSASignaturePadding.Pkcs1`. This decides whether
  the design below works at all, so the code decides it for itself at run time (see
  "Binding") and the manual probe reports it.
- The key signs only. No `IKeyCredential` method decrypts. (`IKeyCredential2`, contract
  19.0, adds `RequestDeriveSharedSecretAsync` for VBS enclaves; not usable here.)
- There is no `IKeyCredentialManagerInterop` and no `KeyCredentialManagerInterop.h` in
  win32metadata, windows-rs or the documentation. The owner-window variants are WinRT
  interfaces that take a `Windows.UI.WindowId`: `IKeyCredentialManagerCreateWithWindowStatics`
  (`RequestCreateForWindowAsync`, IID `30B1B9C9-61EF-43E8-88AC-CC433B38D1A6`) and
  `IKeyCredentialWithWindow` (`RequestSignForWindowAsync`, IID
  `050DEB3B-B19C-4635-9DF6-5650D66C62B1`), both UniversalApiContract 20.0, first listed in
  the SDK release notes for build 10.0.26100.7627 (January 2026). Which Windows builds
  implement them is not documented.
- NCrypt's "Microsoft Passport Key Storage Provider" has no documentation beyond its
  constant (`MS_NGC_KEY_STORAGE_PROVIDER`): key naming, creation and decryption support
  are undocumented. `NCRYPT_WINDOW_HANDLE_PROPERTY` and `NCRYPT_USE_CONTEXT_PROPERTY` are
  documented generally.

## Decision

### 1. One Windows Hello key for the device, through KeyCredentialManager

One Hello key per Gesher device (per Android user, as gatekeeper's secure user id), not
per app or per Android key: Android's auth tokens are device-wide, a confirmation made
for one app releases every app's keys with a timeout, and one key means one enrollment.
`KeyCredentialManager` is used because it is documented; the Passport KSP is not. All
calls are made by the system server (`com.gesher.server.WindowsHelloKey` over
`compat/devices/hello_key_win.cc`), never by app processes, so the key's "application" is
always the same process image and the prompt's foreground rule has one owner.

### 2. Binding: a sealed ECDH key pair, unlocked by the Hello key's unique signature

`com.gesher.server.KeystoreAuthorization` keeps a *binding*
(`$ANDROID_DATA/system/gesher-keystore-binding`): a random id and salt, the Hello key's
name and public key, an EC P-256 public key, and that pair's private key encrypted with
AES-256-GCM under `KEK = HKDF-SHA256(ikm = S, salt, "Gesher AndroidKeyStore binding KEK
v1")`, where `S` is the Hello key's signature over `context || id || salt`.

- A key that requires user authentication is *sealed* to the binding public key:
  ephemeral ECDH P-256, HKDF-SHA256, AES-256-GCM, with the binding id, timeout and
  authentication type as associated data. Sealing needs no prompt, as key generation on
  Android needs none. The app's key store holds only this blob.
- Opening any blob needs the binding private key, which needs `S`, which Windows gives
  only after the user's Windows Hello gesture.
- This is sound only if `S` is the same every time. The code does not trust either
  documentation page: at enrollment and at every later use it accepts `S` only if
  `S^e mod n` equals exactly `00 01 FF..FF 00 DigestInfo(SHA-256) H(message)`, the
  RSASSA-PKCS1-v1_5 encoding, under the recorded RSA public key of at least 2048 bits.
  RSA is a permutation, so exactly one value passes: a signature that passes is the
  unique one and can stand in for a stored secret. A PSS signature never passes; then no
  binding is made and making such keys fails (item 4). The GCM tag of the sealed private
  key is a second, independent check on every use.
- The alternative in the task, a random KEK wrapped to the Hello public key and unwrapped
  by a Hello-gated private-key decryption, is not available: the documented API has no
  decryption.
- Assumption: a unique RSA signature on a message nobody else has seen signed is
  unpredictable (it follows from unforgeability), and HKDF is treated as a random oracle
  over it. `S` is a secret here: it is never written, and is wiped after use where Java
  allows.

Enrollment happens when the first such key of the device is generated or imported: two
Windows Hello prompts (creating the key, then its first signature), owned by the
requesting app's foreground window. A background caller, a cancel or a failure makes that
key generation fail (`ProviderException`; `KeyStoreException` on import).

### 3. Threat model, stated plainly

- Protected: key material at rest. Reading the app's key store and the binding file,
  from this account, another machine or a backup, does not give the keys without a
  Windows Hello gesture on this Windows account (and this TPM when the Hello key is in
  one).
- Not protected: software in memory. After a release, key material is in the system
  server's and the app's memory; the checks of timeouts and per-operation authorization
  are code in Gesher processes. There is no TEE. Malware running as the same Windows user
  can read that memory, replace files, or show its own prompt and ask for the same
  signature. An app can keep a key it was given. Android's guarantee against a
  compromised kernel does not exist here.
- Therefore nothing is claimed beyond software: a future `KeyInfo` must answer
  `SECURITY_LEVEL_SOFTWARE` and `isInsideSecureHardware() == false`; key attestation
  stays unavailable; StrongBox stays unavailable; no biometric class is reported
  (ADR 0091 decision 2 is unchanged).

### 4. Android semantics

| Request | Result |
| --- | --- |
| Key with type `AUTH_BIOMETRIC_STRONG` only (including `setUserAuthenticationRequired(true)` alone, and validity `-1`) | Cannot be satisfied: Windows Hello does not say whether a biometric or the PIN was used. `init`/`initialize` throw `InvalidAlgorithmParameterException` caused by `IllegalStateException("At least one biometric must be enrolled to create keys requiring user authentication for every use")`, as AOSP with no enrolled biometric (`AuthService.getAuthenticatorIds` is empty). Import: `KeyStoreException` with that cause. |
| Type includes `AUTH_DEVICE_CREDENTIAL`, no Windows Hello (or no natives, or no `gesher.keystore`) | The same exceptions caused by `IllegalStateException("Secure lock screen must be enabled to create keys requiring user authentication")`. |
| Timeout > 0 | Released while the last confirmation is no older than the timeout (`age_seconds > timeout` fails, as keystore2). Otherwise `UserNotAuthenticatedException` from `init`. |
| Timeout 0 | `init` begins an operation and succeeds; its handle is what `CryptoObject.getOpId()` returns. The key is released to that operation once, after a confirmation made for that handle. Used before that: `IllegalBlockSizeException`/`SignatureException`/`ProviderException` with an `android.security.KeyStoreException(-26)` cause, as above. |
| The Hello key is gone (`OpenAsync` → NotFound, i.e. a Windows Hello reset), was replaced, or no longer gives the binding's signature | The binding is deleted; every key sealed to it throws `KeyPermanentlyInvalidatedException` from `init`. New keys enroll a new binding. |
| `setInvalidatedByBiometricEnrollment(true)` | Not observable on Windows: adding a fingerprint or face does not change the Hello key. Only biometric-only keys are affected by this flag on Android (credential-capable keys are bound to the root secure id and survive enrollment), and those cannot be made here, so no key exists for which the flag would have to invalidate. |

A confirmation counts as `AUTH_DEVICE_CREDENTIAL` only. A signature obtained for an
operation also counts for keys with a timeout, as on Android.

The binding private key is kept in the system server's memory after a confirmation only
while a key with a timeout could still use it (up to the longest timeout of any key
sealed to the binding), and not at all when there is no such key.

### 5. CryptoObject plumbing

Keys that require user authentication are opaque objects
(`AndroidKeyStoreAuth.AuthSecretKey`, `AuthRsaPrivateKey implements RSAKey`,
`AuthEcPrivateKey implements ECKey`; no format, no encoding). The provider registers
Cipher, Signature and Mac services for exactly these classes (the transformations
`AndroidKeyStoreBCWorkaroundProvider` registers), implementing
`android.security.keystore.KeyStoreCryptoOperation`. Other providers decline the keys by
their own `SupportedKeyFormats`/`SupportedKeyClasses`, so JCA selects these. Each asks
`gesher.keystore` for the key under the rules above and then lets another provider's
primitive do the work in the app's process. Keys without the requirement are plain keys
as before and never reach these services.

Not provided: `KeyAgreement` for such EC keys (no provider accepts the key:
`InvalidKeyException`), `KeyInfo`/`KeyFactory`/`SecretKeyFactory` (not provided for any
key today).

### 6. AuthService

Only the `operationId != 0` path changes. Such a request is accepted when
`KeystoreAuthorization` holds that operation for the caller's uid, and it is confirmed by
the binding's signature (`KeystoreOperations.authorize`) instead of UserConsentVerifier: one
Windows Hello prompt, and `onAuthenticationSucceeded` only when the operation got its
key. Any other operation id still ends with `BIOMETRIC_ERROR_HW_UNAVAILABLE`.

For keys with a timeout an ordinary confirmation (no operation) has to release them too.
`AuthService` is not changed for that: `KeyBoundHello` wraps the `WindowsHelloBackend`
given to it, and while the binding has keys with a timeout it performs the confirmation
as the binding's signature (the same Windows Hello gesture, once). Otherwise, and when
the binding turns out to be gone before Windows asked anything, it is the plain verifier.
Consequence: in that case Windows' prompt does not show the app's message, because
`RequestSignAsync` has no message parameter.

Trust, keyguard and the confirm-credential activity belong to phase 2b
(`feat/hello-device-credential`). Until `trust` reports a secure device, no credential
prompt starts, so this path is not reachable in production from this change alone.

### 7. Migration

The old file format (`GKS1`) does not record whether a key required user authentication,
so the keys that did cannot be identified. Decision: `GKS1` keys are read as they are
and keep working (no user data is lost); each is marked *legacy*, which is written out
with the new format (`GKS2`) and means "authorizations unknown, not enforced". New keys,
including a regenerated key under the same alias, are enforced. Rejected: invalidating
every old key (destroys app data for keys that never required authentication); sealing
every old key (would demand authentication for keys that never asked for it, and prompt
at upgrade). This leaves legacy keys that asked for authentication unenforced, as they
were; that is an open question for maintainers below. A `GKS2` file is not readable by
an older Gesher.

## Open questions for maintainers

1. Legacy keys (item 7): is "keep, marked legacy" accepted, or should an upgrade
   invalidate them so apps re-create them enforced?
2. Enrollment prompts at key generation (item 2): Android never prompts there. Is that
   an acceptable desktop difference, or should enrollment be a separate, explicit step?
3. `KeyBoundHello` changes which Windows prompt an ordinary confirmation shows while
   keys with a timeout exist (no app message). Acceptable?
4. Upstream ADR 0090 (AppContainer isolation): all Hello calls are in the system server,
   so app containers are not involved; confirm the system server stays outside one.

## Not verified

- Everything on Windows: whether the signature is PKCS #1 v1.5 (if it is PSS, no key
  that requires user authentication can be made, and this design must be revisited);
  whether `RequestCreateAsync` and each `RequestSignAsync` prompt; that a Hello reset
  gives NotFound; the key's namespace for an unpackaged process; TPM backing; the
  owner-window variants and the assumption that a `WindowId`'s value is the HWND.
- ART/Conscrypt end to end: provider selection was exercised on a host JDK only.
- RFC 8017 could not be fetched in the authoring environment; the uniqueness argument
  above does not depend on its text.
