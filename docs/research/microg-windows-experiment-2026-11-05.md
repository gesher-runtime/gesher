# microG on Gesher: Windows experiment, 2026-11-05

**Follow-up:** the runtime fixes are now integrated in the working-tree source,
and real FCM delivery passed with a terminated client, after device restart, and
through the normal packaged launcher. See the
[source integration results](#source-integration-and-cold-fcm-follow-up) below.
The initial overlay experiment is preserved here as historical evidence.

## Initial overlay experiment result

**A real FCM message was received by the unmodified ntfy Play APK through the
unmodified microG GmsCore APK running on Gesher on Windows.**

The isolated experiment used a copy of the existing installed Gesher runtime,
with a separately compiled Java overlay. This was not a clean build of the
research branch or a production integration. No personal Google account was
added. The user explicitly approved the Google device registration and test
message traffic.

| Event | Observed result |
| --- | --- |
| Official GmsCore APK verification | SHA-256 matched upstream; APK signing certificate verified with apksigner |
| GmsCore UI | Main settings and Self-Check displayed in native Windows windows |
| Providers | 13 manifest providers published across Binder, including Chimera, accounts, settings and GServices |
| Signature Self-Check | System signature spoofing, microG installed, and microG signature checks passed |
| Google Check-in | Succeeded without a Google account after removing Gesher's synthetic account from the experiment |
| Google MCS | TCP connection and TLS to mtalk.google.com:5228; server LoginResponse received; microG logged in |
| ntfy registration | Real Firebase registration token obtained; test topic subscription completed successfully |
| Message publication | ntfy.sh accepted message Hs6La0K3xd6e at 00:44:39, Asia/Jerusalem |
| microG receipt | Matching DataMessageStanza at 00:44:41.156, category io.heckel.ntfy |
| ntfy receipt | Matching NtfyFirebase Received message at 00:44:41.344; message displayed in ntfy |

The topic was `gesher-microg-e2e-b4e07d4dd30a` and the payload was
`FCM end-to-end test b4e07d4dd30a`. The publisher response, MCS stanza and client
callback all contain the same message ID. The ntfy subscription had
`instant=false`, `usingWebSockets=false`, and its subscriber service was stopped.
This establishes an actual Google FCM delivery. ntfy was running during the test;
delivery to a terminated client process has not been tested.

## Remaining Self-Check limitations

The final screen no longer showed the top-level Self-Check exception. It still
reported the optional Services Framework proxy as missing and battery
optimizations as not ignored. Permission metadata checks remain incomplete:
framework permission parsing encounters a missing boot class, and the
CrossProfileApps system service is absent.

The Companion/Play Store presence and signature checks were green because
Gesher still supplies a synthetic `com.android.vending` package. A real
Companion APK was **not** installed in this experiment. Those green checks do
not establish that Play Store or billing works.

## Changes necessary for the experiment

All implementation changes are in `out/microg-experiment/patch-src`, compiled
into `runtime/experiment-overlay.jar`, before the original `gesher-app.jar` on
the boot class path. The existing runtime installation and unrelated working
tree changes were not edited.

1. Built the real AOSP `com.android.location.provider` shared library from the
   `android-16.0.0_r4` sources and declared it in the isolated system root.
   Without it, the original microG APK failed to load with
   `INSTALL_FAILED_MISSING_SHARED_LIBRARY`.
2. Allowed ActiveServices to start and bind installed GMS packages while keeping
   fail-fast behavior for synthetic packages.
3. Added an opt-in signature substitution for the two microG package names,
   conditioned on the official microG signer certificate fingerprint. The
   downloaded artifact was also verified externally with apksigner and SHA-256.
4. Returned a local ContentProviderHolder for a same-package provider that has
   not yet been published, allowing ActivityThread to install it. The previous
   behavior waited for its own bindApplication and timed out after 120 seconds.
   Observed settings launch time changed from 120834 ms to 500 ms.
5. Used the current DeviceServices implementation that returns Build.UNKNOWN
   for unavailable serial numbers. The copied installed binary had returned
   null, causing ProfileManager to fail.
6. Suppressed the synthetic `user@gesher.local` account when a real GMS APK is
   present. Otherwise microG tried to authenticate its dummy password and
   stopped with BadAuthentication before Check-in.
7. Implemented ActivityTaskManager.getDeviceConfigurationInfo from Gesher's
   existing DeviceProfile configuration. Its null default prevented Check-in.
8. Added a DeviceIdleController Binder stand-in so Self-Check can report the
   unavailable battery exemption instead of throwing a null-pointer exception.
   Permission metadata support is experimental and still incomplete.

The harness uses a separate ANDROID_DATA, system root, Binder namespace and log
directory. GESHER_LOGS must use a drive-less Android path: a Windows drive
prefix caused Win32 error 123 when the system server spawned app processes.
The current implementation still has Gesher's shared UID and permissive
permission model. This experiment does not validate account isolation or safe
deployment with personal Google credentials.

## Artifacts and reproduction

The experiment directory is
`C:/Users/otzaria/Documents/gesher/out/microg-experiment`.

- `fcm-evidence-redacted.json`: matching publisher, MCS and client evidence,
  excluding registration/authentication tokens.
- `fcm-received.png` and `selfcheck-final.png`: actual screenshots.
- `runtime-manifest.json`: runtime and APK hashes.
- `patch-src/`, `build-patch.ps1`: source and build script for the Java overlay.
- `device.ps1`: isolated device lifecycle, settings, Check-in and ntfy commands.
- `enable-cloud-test.py`: enables Check-in and Cloud Messaging only in the
  experiment profile.
- `microg-experiment-evidence.zip`: source overlay, scripts, this report,
  screenshots and redacted proof; excludes APKs, runtime binaries and raw logs.

The GmsCore input is release `v0.3.17.252432`, file
`com.google.android.gms-252432034.apk`, SHA-256
`e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35`.
The verified signer SHA-256 is
`9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165`.
The ntfy input is the official `v1.25.2` Play release APK, SHA-256
`86fb24d185c517a63e10dd311765db69460dbc45a6403b66edd1898b86c18df1`.

For another run, build the overlay, start the isolated device, run Check-in,
launch ntfy and publish to its existing test topic. A receive test should verify
the same message ID in both MCS DataMessageStanza and NtfyFirebase callback logs.
Raw microG logs contain test device credentials and should not be published.

## Review of the supplied research branch

The supplied branch was inspected at
`6b69e2af4787a212f1d113131d19fc152a83f712`. It is useful as a draft, but the
following issues prevent treating it as a working integration:

- `isOfficialMicrogCertificate` is defined but never invoked by
  `maybeSpoofPackageInfo`. The actual gates are a package-name allowlist and a
  requested permission; the claimed signer restriction is not enforced.
- `git apply --check` rejects `PackageManagerService_microg.patch` as a corrupt
  patch at line 9. It also refers to `generatePackageInfo`, while the actual
  ApkPackage method is `packageInfo`.
- Track 52 invokes `verify-gms-fallback`, `verify-spoof-gate` and
  `verify-microg-apk`; none is implemented in RuntimeCommands in that commit.
- The packaging script stages files but does not invoke the runtime. Its
  permission XML lookup points beside the script, whereas the supplied XML is
  under `system/etc/permissions`.
- The branch does not address the shared-library, provider initialization,
  dummy-account or device-configuration failures encountered in the live test.

The experiment demonstrates feasibility for real FCM. It does not establish
Gmail, Maps, billing, account login or broad application compatibility.


## Source integration and cold FCM follow-up

The second stage built `gesher-app.jar` from the current repository Java source,
including generated Binder stubs. The temporary `experiment-overlay.jar` was
removed from the boot class path. The existing installed native runtime was
reused in the isolated copy; this was not a rebuild of all native libraries or
of the Windows installer. Pre-existing window-related working-tree edits were
left intact. No commit, push or PR was created by this experiment.

The user-supplied GitHub Actions `framework-classes` artifact matched SHA-256
`9181f8179dfe6e0c02c9732378bfe0ab3476988727996f0d25628ed3e9d84d6e`.
It contained the exact framework classes and core module. Android class names
that differ only in case collided when extracted on Windows; the successful
build used the original class archive. The genuine AOSP location provider
library was compiled from `android-16.0.0_r4` and staged with its declaration.

The integrated implementation includes installed-GMS service resolution,
same-package local provider acquisition, real device configuration, suppression
of the synthetic account, a DeviceIdleController stand-in, opt-in addon loading,
and a targeted BOOT_COMPLETED broadcast to restore microG's enabled services.
The packaged setup helper stages the official APKs in the profile; the normal
launcher reads the opt-in marker and restores its environment after invoking
child processes. Disabling the marker was tested: the next device start had
only ntfy installed, no microG app process, and retained the APK/database files.

Full APK verification initially exposed a missing native Android incremental-fs
method. The final source bundles official Android apksig 8.13.0 instead. It
verifies the signed contents and current signer, then restricts substitution to
the pinned official microG key, the Services/Companion package names, a declared
FAKE_PACKAGE_SIGNATURE permission and explicit opt-in. Arbitrary manifest
fake-signature metadata is ignored. Both signature APIs share that policy.

### Measured real delivery

All times below are on 2026-11-05 in Asia/Jerusalem (UTC+02:00).

| Case | Publisher message ID | Published | microG MCS receipt | ntfy Firebase callback |
| --- | --- | --- | --- | --- |
| Client process terminated | Pxy0YE3tDAVU | 01:24:18.839 | 01:24:19.563 | 01:24:20.090 |
| Full device restart, client unopened | pK6luRuCMvTG | 01:27:35.673 | 01:27:36.602 | 01:27:37.100 |
| Normal packaged launcher and real installer registry | BUH7ANp8OqKX | 01:33:24.241 | 01:33:25.038 | 01:33:25.485 |

The first successful case killed ntfy PID 27084 and recorded no ntfy process
before publication; FCM started PID 27824. After the full restart only microG
PID 23432 was present before publication; FCM started ntfy PID 28124. The normal
launcher case similarly started ntfy PID 22200 from an absent client. Each
message ID appears in the publisher response, the Google DataMessageStanza,
ntfy's Firebase callback and its notification display log. The subscription
remained non-instant with WebSockets disabled; the subscriber service was
stopped. No client activity was opened to receive the successful cold messages.

An earlier publication immediately after switching runtimes did not arrive.
ntfy was initialized once under the new runtime, then terminated before the
successful cold test. The exact cause of that initial miss was not established;
it is not counted as a pass. Subsequent device restart and packaged-launcher
cases required no client reopening or manual Check-in broadcast.

The real Self-Check window was inspected again on the source runtime. Signature
spoofing, Services presence and its signature passed. Companion checks were
still satisfied by the synthetic package; a real Companion/GSF proxy was not
installed. Permission metadata and CrossProfileApps remain incomplete, and the
battery exemption was false. This is an FCM milestone, not completed sign-in,
account isolation, store, billing, location or Play Integrity validation.

### Local validation and evidence

- App Java runtime, generated service stubs, genuine location library and guest
  tests built successfully against the supplied framework/core classes.
- MicrogTest passed with spoofing disabled and enabled, including a genuine
  GmsCore APK, non-microG signers, unsigned APKs and a copied official APK whose
  classes.dex content was changed while retaining its original signing block.
- SigningTest passed for ordinary v2 APKs, unsigned APKs and rotated signing keys.
- Packaged launcher installation/startup, opt-in environment restoration and
  addon disable/startup were exercised in the isolated profile.
- Bash/PowerShell parsing and git diff whitespace checks passed. The complete
  Windows CI and installer compilation remain unrun locally.

The source-built Java jar SHA-256 is
`df57252359e882e718f3b8a60d3e2ffdfbbbea534cdb0f5b352fb7704ce1c1fa`.
The evidence directory contains `cold-fcm-evidence-redacted.json`,
`restart-fcm-evidence-redacted.json`, `launcher-fcm-evidence-redacted.json`,
`disabled-addon-evidence.json`, `offline-checks.txt`,
`source-runtime-manifest.json` and `source-selfcheck.png`.
`microg-source-integration-evidence.zip` packages these selected artifacts,
reproduction helpers and relevant source files; it excludes raw device logs,
account/token stores, APKs and runtime binaries. All isolated runtime processes
were stopped after testing. The installed user runtime was not replaced.

The remaining implementation sequence and installation commands are in
[the runtime integration guide](../microg.md).

## Expanded protected runtime validation

The expanded implementation was validated on Windows x86_64 on 2026-11-05.
The Java runtime and test APKs were built from an isolated source checkout;
there was no Java overlay. Existing native ART binaries were reused, and the
new credential bridge was compiled locally with Clang for Windows. This is not
a local full CMake/Windows-installer build. The production CMake, CI artifact
collection and installer now include that bridge and the AOSP location library.

The final `gesher-app.jar` SHA-256 is
`765456f5f2f2e91fd5c35f0ba7d23a365f02ca7ab1295b645cade6bb65764596`.
Artifact hashes and token-free measured results are committed in the
[validation record](microg-integration-validation-2026-11-05.json).

### Security and lifecycle results

The committed `tests/windows/run-microg-security.ps1` passed all 36 checks on
actual ART in a fresh protected profile without installing or executing Google
APKs. Optional official GmsCore input was parsed for metadata tests only.
Coverage includes:

- Verified APK content, copied-install integrity, signer rotation capabilities,
  durable distinct UIDs and authenticated process attachment.
- Real test-authenticator delegation, token cache/invalidation/expiry, explicit
  client token grant/revoke, interactive authenticator continuation, opaque
  one-use sessions and stripping credentials from non-token results.
- Positive native DPAPI persistence of fake passwords/user data/tokens across a
  second SystemServer process, native non-server role rejection and no plaintext
  fallback. The host JNI tables/OnLoad use the Windows ABI; managed native
  entry points use the System V ABI.
- Attached app attempts to read hidden accounts or owner secrets, spoof
  packages/PendingIntents, access private components/providers, mutate Settings,
  post/listen without approval, schedule another app's jobs or change global sync.
- Actual own-app JobService execution and completion, namespace isolation,
  job/sync system-binding cleanup with caller identity restoration, and a single
  app PID observing notification permission denied, granted, then revoked.
- Manifest receiver batch keepalive and authenticated completion, timeout/death
  generation races, service recovery, stored notification clicks through fresh
  service controllers, URI grant/revocation and observer callback revalidation.
- Delayed content-triggered sync retains notifier code identity, URI write
  authorization and account visibility; revoked/replaced callers cannot cause
  hidden-account work.

The setup helper also successfully verified and staged real official Services,
Companion and GSF Proxy APKs. The new Self-Check activity launched, but visual
capture failed with `CreateForMonitor 0x80070057` and then `GetCursorPos access
denied 0x80070005`. No new complete visual Self-Check pass is claimed. Android
battery exemption remains unsupported as described in the guide.

### Final real FCM results

Both tests below used the final jar above, unmodified official microG and ntfy
APKs, default message priority, a non-instant subscription and no personal
Google account. Times are local Asia/Jerusalem (UTC+02:00), on 2026-11-05.

| Case | Publisher ID | Published | MCS receipt | Firebase callback | ntfy display log |
| --- | --- | --- | --- | --- | --- |
| ntfy process absent | 8dFMHhK4LrOI | 04:29:11.239 | 04:29:13.682 | 04:29:14.705 | 04:29:14.721 |
| microG crashed/recovered; ntfy absent | werouUALwaPw | 04:29:51.270 | 04:29:52.394 | 04:29:53.222 | 04:29:53.234 |

Before each publication, device status listed only microG. The first message
started ntfy PID 9860. Killing only microG PID 28136 left SystemServer PID 33884
running; normal service recovery started microG PID 18588 and MCS logged in
automatically. The second publication then started ntfy PID 31920. Matching IDs
appear at the publisher, MCS input, Firebase callback and ntfy display log.
All isolated test device processes were stopped afterward.

One earlier publication, `M2X7ncstZR1i` at 04:19:57 on the preceding authorization
build, was absent from MCS/client logs during the 40-second observation window.
The MCS channel subsequently acknowledged a heartbeat; there was no duplicate
microG process or recorded job/identity rejection. A normal-priority retry on
the same unchanged connection, `ZzO9iI8Lj0dK` at 04:27:10, arrived successfully.
The cause of the missing publication was not established. Its failed observation
is retained in the validation record, rather than counted as a pass.

These are measured integration cases, not delivery/soak guarantees. Personal
Google sign-in, real fused location, network-loss recovery, long idle, multiple
independent clients, Play Integrity, proprietary billing and DRM remain
unvalidated. Android API identity and DPAPI checks do not provide a Windows OS
sandbox against code running with the same user's host privileges. See the
[current integration guide](../microg.md) for supported paths and limitations.

## Authenticator discovery follow-up

The official microG v0.3.17.252432 APK declares its GoogleLoginService as private,
without `BIND_ACCOUNT_AUTHENTICATOR`. Gesher's original discovery predicate
incorrectly excluded it. A metadata-only ART regression reproduced the absent
descriptor on the earlier build and verified it after accepting enabled private
authenticators. Exported services still require the platform bind permission,
Google account types retain the verified official-owner gate, and disabled
services and manifest-disabled owner applications are excluded. XML custom-token
flags and literal Boolean compatibility metadata are handled without mistaking
resource-reference metadata for a Boolean.

The follow-up jar and fixture passed all 37 offline integration checks. Exact
hashes are in the [separate validation record](microg-authenticator-validation-2026-11-05.json).
The active interactive sign-in profile was not replaced or copied for these
tests, and the tests read no personal accounts, cookies or credentials.

This fixes discovery, not the observed consent-page stall. The completion path
also requires a general WebView bridge threading correction: microG's JavaScript
`closeView` callback synchronously reads CookieManager, while Gesher currently
runs that callback on WebView2's thread and returns null for synchronous cookie
reads there. No successful real Google account registration or token use is
claimed by this follow-up. The earlier FCM measurements remain tied to their
explicitly recorded prior artifact hash.

## WebView login completion follow-up

Two general host bugs prevented supported completion paths: JavaScript-interface
methods ran on WebView2's STA, where a synchronous cookie read cannot wait for
itself, and primary-frame same-document fragment changes did not deliver
Android's `onPageFinished`. The latter matters to microG's `#close` completion
path. These are host compatibility fixes, not a claim that the earlier consent
screen trace reached either completion path.

HTTP(S) bridge methods now run in order on a Java worker while JavaScript keeps
its synchronous return semantics. A cheap native `prepareCall` stores bounded
arguments locally and produces a random, expiring, one-use ticket. Only this
ticket enters the same-origin XHR. The native broker consumes it before Java
dispatch; `finally` retires it even when browser policy rejects the XHR. Real
browser CSP and synchronous-XHR restrictions remain in force.

A live destruction test found that WebView2 could release the original POST to
the loopback origin when a controller closed, despite a preinstalled rejection
response and subsequent request mutation. The rejected development candidate
sent 116 bytes of synthetic fixture arguments. No personal data or Google login
was involved. The final protocol keeps arguments out of HTTP entirely and parks
a retiring controller until outstanding response acknowledgments allow closure.
The original failing evidence is retained alongside the successful regressions;
this is not an assertion of zero packets after arbitrary browser/host crashes.

The final four actual WebView2 cases passed: HttpOnly cookies and ordered
non-STA void/nonvoid calls; nine fragment/history callback steps; dedicated,
shared and service-worker boundaries with fresh valid tickets; and a view
destroyed while its Java callback was active. The origin counters remained zero
for requests, body bytes, reserved paths and private fixture payloads. The server
leak detectors were independently exercised with deliberate local POST controls.
The native Release helper suite also exercises CNG ticket creation, private
payload retention, cancellation, expiry, generation and prepared-call bounds.

The same final Java jar/native bridge also passed all 37 account/security
integration checks. Exact artifact hashes and the measured boundaries are in
the [login validation record](microg-login-validation-2026-11-05.json).

The native component is built from the reviewable `gesher_webview_bridge`
shared CMake target and loaded by absolute path beside `gesher-app.jar`.
The validated local runtime retains the original working executable. A separate
new full-native baseline failed controller creation before loading pages, so it
is not the runtime deployed for this follow-up. Long nested fixture paths also
failed controller creation; the committed runner uses fresh short workspace
profiles and records their locations. The complete installer pipeline was not
run locally.

A separate manual `googleaccount` app uses real AccountManager delegation, then
requests an email-scope access token and verifies HTTP 200, an account ID and the
selected account's email against Google's fixed HTTPS userinfo endpoint. It
starts no login or network action until the user presses a button, and logs no
credentials. This is a manual acceptance tool, not a recorded successful Google
login or validation of every other application's scopes/SDK. Account visibility
and Google's developer-client policy remain applicable.
