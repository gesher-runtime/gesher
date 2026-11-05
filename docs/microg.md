# Optional microG runtime integration

Gesher can load the unmodified official microG Services, Companion and Services
Framework Proxy APKs as optional system addons. The integration includes real
authenticator delegation, protected account storage, runtime permissions and
checks on application Binder callers. It remains an experimental, single-user
Windows runtime. Android API checks do not provide an operating-system sandbox
for arbitrary APK or native code.

The expanded runtime passed 36 offline/integration checks and real FCM delivery
to a terminated client, including after microG crashed and automatically
reconnected. These results do not establish a complete Self-Check pass or
personal Google-account sign-in. See the [experiment and measured evidence](research/microg-windows-experiment-2026-11-05.md#expanded-protected-runtime-validation)
and the [sanitized validation record](research/microg-integration-validation-2026-11-05.json).

## Install, enable and remove

Use the matching native runtime, with the existing `GESHER_UID` and
`GESHER_BINDER_UID` identity hooks, `libgesher_account_crypto.so` and the matching
`libgesher_webview_bridge.so`.
Updating only `gesher-app.jar` in an older runtime is insufficient. The runtime
includes AOSP's real `com.android.location.provider`
shared library and declaration, the privileged-permission allowlist and the setup
helper. It does not bundle the microG APKs.

Obtain the official APKs and keep them unmodified. Stop the device before
replacing or removing addon files:

```powershell
gesher-app shutdown
& "$env:LOCALAPPDATA/Programs/Gesher/gesher-app-microg.ps1" `
    -GmsCoreApk 'C:/path/to/com.google.android.gms.apk' `
    -CompanionApk 'C:/path/to/com.android.vending.apk' `
    -GsfProxyApk 'C:/path/to/com.google.android.gsf.apk'
gesher-app start
```

`-GmsCoreApk` is required when enabling; `-CompanionApk` and `-GsfProxyApk` are
optional. These are real APK slots, including a real Companion instead of a
synthetic package-presence check. Companion installation does not supply the
proprietary Play Store, billing or DRM implementations.

The helper verifies the expected manifest package, signed APK contents and
official signer offline before replacing any supplied slot. It stages files as
`system/priv-app/<package>/base.apk` under the selected profile and writes
`microg.enabled`. Startup verifies the fixed slots again before parsing/loading
their application code; an unexpected or modified APK stops startup.

The normal launcher uses `%LOCALAPPDATA%/Gesher` and enables
`GESHER_MICROG_SPOOF=1` when that profile's marker exists. An explicit environment
value, including `0`, takes precedence. The helper's `-ProfileRoot` and
`-RuntimeDir` support an isolated test harness; the harness must launch the same
profile. They do not change the normal launcher's profile selection.

Open microG Settings to configure Google device registration, Cloud Messaging
and accounts. Staging the APKs does not enable those settings. On device startup,
Gesher sends verified Services `BOOT_COMPLETED` so microG can restore services
enabled in its own settings. Gesher must be running to receive push.

Disable loading on the next device start while retaining APKs and data:

```powershell
gesher-app shutdown
& "$env:LOCALAPPDATA/Programs/Gesher/gesher-app-microg.ps1" -Disable
gesher-app start
```

To remove the three addon APK slots instead, stop the device and run the helper
with `-Remove`. Both operations retain application and account data; neither is
an account-data wipe. Clear any explicit `GESHER_MICROG_SPOOF` override before
relying on the marker's disabled state. Without an installed real GMS package,
Gesher retains its synthetic GMS/dummy-account compatibility path. Synthetic
sign-in, token and push tests are separate from real microG/Google results.

## APK verification and certificate compatibility

Only `com.google.android.gms` and `com.android.vending` can receive Google's
compatibility certificate. The feature must be enabled, the APK must request
`android.permission.FAKE_PACKAGE_SIGNATURE`, and official Android `apksig`
verification must validate its complete signed contents and pinned current
microG signer:

```text
9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165
```

Services and Companion require a verified v2/v3 signing scheme. The historical
GsfProxy v0.1.0 is v1-only: the loader accepts only the verified official signer
and this exact artifact SHA-256:

```text
86891b174301f06a1c84187b545a0a2a57044c6b768f3e84e865908743349692
```

This exception does not authorize other v1 APKs, and GSF receives no substituted
Google certificate. Manifest fake-signature metadata cannot select a
certificate or authorize substitution. `GET_SIGNATURES`,
`GET_SIGNING_CERTIFICATES` and package enumeration use the same gate. Permission
ownership and stored capabilities use verified installed code identity rather
than the substituted compatibility certificate.

Ordinary APK installation verifies claimed signatures and split signers,
rechecks copied code, and enforces verified signing continuity on updates.
Unsigned developer fixtures remain supported; replacing their code invalidates
old code-bound capabilities. Loaded APK files must remain unchanged. The
installer prevents installing an application as the reserved `android` owner.

## Permissions and application identity

With `GESHER_MICROG_SPOOF=1`, each package receives a durable distinct Android
UID. The registry persists across device restarts and does not recycle allocated
identities. Binder attachment binds the expected process start, PID and UID to
its package; services authenticate this mapping instead of trusting a claimed
package string. Unknown, stale and mismatched callers fail closed. This policy
remains enabled even if GmsCore is absent or removed.

Permission metadata comes from framework resources and installed manifests.
Normal permissions require a declaration, dangerous permissions start denied,
and signature/privileged permissions require verified ownership or the explicit
verified-addon allowlist. A runtime permission request uses the host's
default-deny confirmation dialog and returns the framework grant result.
Unavailable confirmation denies the request. Cache invalidation reaches existing
app processes, so grants and revocations do not require restarting the client.

The trusted host can grant or revoke a requested dangerous permission for user 0:

```text
com.gesher.server.RuntimeCommands pm grant PACKAGE android.permission.POST_NOTIFICATIONS
com.gesher.server.RuntimeCommands pm revoke PACKAGE android.permission.POST_NOTIFICATIONS
```

These are host-runtime commands, invoked with the running device's Binder
namespace, Android environment and boot class path; they are not `gesher-app`
launcher subcommands. `RuntimeCommands` connects as the authenticated host shell.
An application cannot grant itself permissions through the corresponding Binder
API.

Activity/service access enforces caller attribution, exported components and
declared permissions. Protected broadcasts and cross-app FCM delivery enforce
sender authorization. Providers enforce authority ownership and access
permissions. Explicit URI read/write grants support exact/prefix scope and
revocation for the device lifetime; they are bound to installed owner and target
code. Location access requires the authenticated caller and a current runtime
grant, including when delivering updates.

Job scheduling and enqueue authenticate the caller as the declared
`BIND_JOB_SERVICE` service's owner. Namespaces scope that owner's jobs; they do
not select another package. Cancellation and queries stay package-scoped, while
global administration and `scheduleAsPackage` require the host. Unknown remote
callers fail closed. Job callbacks and persisted records check installed UID/code
identity, including when a pending service connection finally delivers job
parameters after an APK update.

Content observers require URI read access at registration and delivery;
`notifyChange` validates caller attribution and write access for the entire URI
batch before dispatch. Network-sync notifications retain the original notifier's
identity and account visibility when deferred, rather than inheriting the
server's authority. Reading sync settings requires `READ_SYNC_SETTINGS`, and
changing account or global sync settings requires `WRITE_SYNC_SETTINGS`.

## Accounts and credentials

Accounts belong to the installed authenticator's package and code identity.
Only that owner or the trusted host can read passwords/user data or mutate
protected account secrets. Enumeration respects per-client visibility;
visibility alone does not grant a normal authentication token.

Token requests bind the real declared authenticator service and retain the
authenticated requesting application's identity. Normal token access requires
an explicit client/token-type grant; interactive access uses host credential
consent. Authorization is checked before using the cache or calling the
authenticator, and rechecked when the result returns. Custom-token caches are
scoped to client code identity and authenticator-provided expiry. Interactive
authenticator continuations are validated. Add/update session APIs return
opaque, expiring, single-use handles bound to the requesting client and current
authenticator; private session bundles stay inside the server.

Passwords, user data, normal tokens and their grants persist in
`data/system/accounts.sealed`, protected with Windows user-scoped DPAPI and
transactional replacement. There is no plaintext fallback if the native bridge,
decryption or a write fails. Legacy unowned plaintext records are sealed for
recovery and require accounts to be re-added before access; the runtime does not
assign them to whichever authenticator happens to be installed later.

These account paths have been exercised with test authenticators and fake
credentials. They do not validate microG's embedded Google login flow or the
safety of entering a personal Google password into this runtime.

Authenticator discovery accepts enabled private services and enabled exported
services protected by `BIND_ACCOUNT_AUTHENTICATOR`; the Google-type signer gate
still applies. A follow-up regression against the unmodified official APK found
that requiring this permission on private services excluded microG's actual
GoogleLoginService. The correction passed the expanded 37-check offline suite,
including disabled services/apps, unsafe exports and forged Google owners.
See the [authenticator validation record](research/microg-authenticator-validation-2026-11-05.json).

Manual account registration and real Gmail mailbox access have succeeded on the
recorded experimental Windows runtime. The original Google-signed Gmail APK
displayed the existing microG account and loaded its mailbox in a fresh Gmail
profile; no existing mail/app data was imported and no individual message was
opened. This validates that client path on the recorded local tuple. The native
binary reuses a broader cached baseline, rather than a clean build of only this
PR's sources. Two host completion bugs have been corrected: HTTP(S)
JavaScript-interface callbacks now use a
serialized worker while WebView2 remains free to retrieve real login cookies,
and same-document fragment navigation delivers Android's `onPageFinished`
callback (used for completion URLs such as `#close`). The Java jar and its
shared WebView bridge must be updated together. The new bridge preserves browser
CSP/synchronous-XHR restrictions and fails honestly when the page blocks it.

`tests/apps/googleaccount` provides a separate manual client. Its first button
uses the real authenticator through AccountManager; its second requests the
basic email scope and verifies the resulting token with Google's fixed HTTPS
userinfo endpoint. Success requires HTTP 200, an account ID and the selected
account's email. It initiates neither authentication nor network requests on
launch and never logs or stores tokens. This validates that client/scope only,
not every application's Google Sign-In SDK, billing or Play Integrity. Google
may reject an unregistered developer client's scope request.

The manual client must be genuinely developer-signed: Google client identity
includes a certificate digest. Its build now retains a unique local signing key
and verifies the APK before publishing the final output. The first unsigned
manual client registered an account but received Google's `BAD_REQUEST` during
token issuance. See the [signed-client follow-up](research/microg-signed-client-validation-2026-11-05.json).
Signing supplies the missing identity field; it does not itself establish that
Google accepts a developer client or scope. The subsequent manual retry returned
`UNREGISTERED_ON_API_CONSOLE`. The follow-up uses the original Google-signed
installed Gmail APK as an existing client. See the
[Gmail acceptance record](research/microg-gmail-validation-2026-11-05.json) for
the three runtime fixes, actual mailbox loading and validation limits. The
follow-up also reuses the existing pinned MediaStore contracts from the stability
branch: Gmail's new-message screen calls `getPickImagesMaxLimit()`. The updated
local build opens that screen without the previous linkage failure. A later
contacts task exposed a missing standard Apache library JAR in the experiment;
completing that existing runtime artifact removed the background failure. The
composer remained active during the recorded observation, and the user then
manually sent a message and reported success. The agent entered no message text
and sent no mail; attachments and independent delivery tracing remain untested.
See the [composer validation record](research/microg-compose-validation-2026-11-05.json)
and [shared-library follow-up](research/microg-http-library-validation-2026-11-06.json).

## Notification access

Application notification mutation and queries authenticate the owning package.
In protected mode, denied or revoked `POST_NOTIFICATIONS` suppresses notification
content in toasts, listeners and active-notification queries. Foreground-service
bookkeeping continues independently. Revocation removes existing stored/displayed
content.

Notification-listener access is a separate special-access decision, denied by
default even when a manifest declares a listener. The host grants or revokes it
through the real notification service API:

```text
com.gesher.server.RuntimeCommands notification allow-listener PACKAGE/CLASS
com.gesher.server.RuntimeCommands notification deny-listener PACKAGE/CLASS
```

Invoke these through the configured host runtime as described above. Grants are
persisted and bound to verified signer identity, or exact code for unsigned
developer APKs. Listener registration, reads and cancellation require an
authenticated owner and a currently authorized registered listener token;
ordinary dangerous-permission approval does not authorize a listener.

Stored notification content/action PendingIntents retain creator UID and code
identity. Tests recreate services with empty in-memory notification and
PendingIntent state, then deliver both restored clicks through the real dispatch
path. Changed creator code cannot load or consume those stored capabilities.

## Build and offline validation

Build Java against the pinned framework and Android core module, then assemble
it with the matching rebuilt native runtime:

```bash
app/build.sh out/app --framework-classes out/framework-classes.zip --core-classes out/coremod
tests/dex-services/build.sh out/app --framework-classes out/framework-classes.zip --core-classes out/coremod --app-classes out/app/gesher-app-classes.jar
tests/apps/devsvc/build.sh out/app --framework-res out/framework-res.apk --framework-classes out/framework-classes.zip --core-classes out/coremod
tests/apps/microgprobe/build.sh out/app --framework-res out/framework-res.apk --framework-classes out/framework-classes.zip --core-classes out/coremod
```

Use the actual framework-res/class/core output from the pinned AOSP build; the
paths above are placeholders for those artifacts. On Windows, use the original
framework class archive directly: extracting Android class names that differ
only in case loses classes on a normal Windows filesystem. With the pinned AOSP
location sources present (`AOSP_DIR` or `third_party/aosp`), the app build also
emits `com.android.location.provider.jar`. It bundles apksig 8.13.0.
`APKSIG_JAR`, `ANDROIDX_ANNOTATION_JAR`, `R8_JAR` and
`BUILD_TOOLS_DIR` can select downloaded build tools.

Run the source-backed security suite against an assembled runtime directory:

```powershell
powershell -File tests/windows/run-microg-security.ps1 -Dir 'C:/path/to/runtime'
```

The runner accepts `-ServicesDex`, `-DevsvcApk`, `-ProbeApk` and `-EvidenceDir` for
separate build outputs. It creates a fresh protected profile and isolated device.
Source-built fixtures cover DPAPI account persistence/restart, attached-app
rejection paths, owner secrets and PendingIntent preservation, and the same
app PID observing POST denied, granted, then denied. Native consent dialogs are
not automatically accepted. An optional local `-GmsCoreApk` with
`-RequireMetadataSuites` adds offline real-manifest tests; no Google APK code or
Google network access is needed for the base security run.

Track 52 invokes this isolated security runner and also runs `MicrogTest` for
opt-in, signer/integrity rejection and the synthetic fallback. Optional `GESHER_MICROG_TEST_APK`,
`GESHER_MICROG_TEST_COMPANION_APK` and `GESHER_MICROG_TEST_GSF_APK` add official
artifact checks. Synthetic compatibility track 93 does not establish real FCM,
real sign-in or microG fused-location operation.

Local integrated tests passed real test-authenticator delegation, token
cache/invalidation, interactive continuation, custom-token expiry and opaque
sessions. Actual DPAPI storage and restart recovered fake passwords, user data
and tokens. Attached probe runs passed while the owner confirmed unchanged
secrets/authenticator counters and its original PendingIntent capability.
Offline notification policy, explicit listener decisions and stored-click
restoration also passed. Offline job tests passed owner/namespace isolation,
unknown PID/UID rejection, legitimate scheduling and pending-bind code-replacement
checks. The final integrated run also passed actual attached JobService work,
content observer/notification denials and delayed sync authorization, including
grant revocation between observer callbacks. All 36 checks passed on the same
source-built Java runtime used for the final real FCM cases.
A complete Windows CI/installer pass remains separate from these targeted results.

## Current limits and trust boundary

- Only Android user 0 exists. There are no work profiles or cross-user account,
  component or URI grants.
- Automatic URI grants from Intent flags, URI owner-token grants and persistable
  URI permissions are unsupported. Persistable and owner-token grant APIs fail
  explicitly; automatic Intent flags do not create grants. A URI's presence
  alone does not authorize access.
- The device-idle service reports no Android battery-optimization exemption;
  Windows does not supply Android Doze. The expanded runtime has not completed a
  new full visual Self-Check run. Earlier green signature checks and synthetic
  Companion checks must not be presented as a complete current pass.
- Personal Google sign-in, real microG fused location, Play Integrity, proprietary
  billing and DRM are unvalidated. FCM still needs network loss/recovery, long
  idle and multiple independently registered client coverage. The final runtime
  passed cold delivery and automatic microG crash recovery. An earlier
  publication was not observed in MCS within 40 seconds; a retry on that same
  unchanged connection succeeded. Its cause remains unresolved, so the measured
  passes do not establish reliable long-term delivery.
- Gesher does not place APK processes in a Windows OS sandbox. APK/native code
  runs with the Windows user's host privileges and can access host resources or
  execute outside these Android service checks. Durable Android UIDs and DPAPI
  protect the implemented API/storage paths; they do not isolate hostile code
  running as that same Windows user. Use trusted APKs and test accounts for this
  integration. Personal credentials are not an established supported deployment.
