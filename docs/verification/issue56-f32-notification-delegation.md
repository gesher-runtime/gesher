# Issue 56 F32: notification delegation

This increment implements notification delegation in the existing notification
service. It does not complete the F32 advanced-notifications item. The base is
`473beec761bd3005a23a8f746a6ba9216343f39d`; no open PR in the verified 57-PR snapshot
owned `NotificationManagerService.java`, `NotificationStore.java`, or delegation.

## Implemented contract

`setNotificationDelegate` and `getNotificationDelegate` now persist a source
package's delegate. A null delegate revokes the grant. An unknown delegate leaves
the previous preference unchanged. `canNotifyAsPackage` returns true for the
calling package itself or a current explicit delegate. Posting and cancellation
authenticate the attached Binder process before accepting the claimed caller.

Delegated notifications retain the target package and its UID, the actual
posting package (`opPkg`), and the posting PID. The existing channel policy,
owner `POST_NOTIFICATIONS` decision, toast publication, listener notifications,
and cancellation route continue to process those records. The delegate's own
posting permission does not gate publication for an authorized owner.

Owners can query their notifications, and delegates can query notifications they
posted for another package. Queries return notification copies and remove the
allowlist token. A delegate cannot cancel a notification posted by its owner or
another delegate. Revocation prevents subsequent delegated operations while
existing posts remain queryable by their original poster. Owners can still
cancel them. Channel read APIs accept explicit delegation; channel creation and
deletion continue to require ownership.

Android uses different authorization for different shared-UID operations:

| Operation for another package with the same authenticated UID | Android contract |
| --- | --- |
| `canNotifyAsPackage`, without an explicit grant | False |
| Post for the co-owner | Allowed |
| Query channels using the posting package as `callingPkg`, without a grant | SecurityException |
| Cancel an owner-origin notification | SecurityException because `opPkg` differs |
| Query the delegate preference while claiming the co-owned source package | Allowed by the source ownership check |

The service preserves these distinctions. Legacy mode assigns unrelated apps
UID 10000, so equal legacy UIDs do not establish shared ownership.

## Persistent ownership and lifecycle

`NotificationDelegates` writes an `AtomicFile` record per source package. It
publishes a changed grant in memory only after the durable write succeeds.
Records include source/delegate package identity, UIDs, the existing package
installation-owner tokens, and the delegate's code continuity identity.

Android's delegate UID changes after the ordinary uninstall/reinstall observed
in the reference run. Gesher retains package UID tombstones. Its existing
`ApkPackage.enabledStateOwner()` UUID therefore also gates delegation: a later
installation with the same name and UID cannot inherit the previous grant.
Uninstalling the source removes its preference. Uninstalling a delegate leaves
the configured name visible to the owner, as Android does, but the replacement
installation needs an explicit new grant.

Verified updates retain the installation owner. This relies on the existing
Installer's verified signer/update-continuity checks; the delegation change does
not implement a second package installer or UID allocator. Unsigned developer
code has no verified update continuity, so its exact code digest additionally
gates an incoming grant. A changed unsigned APK cannot retain that authority
merely because its package's installation marker survived an update.

The original delegation branch's `NotificationStore` version4 retains `opPkg` so original-poster authorization
also applies to Windows-retained notifications after a server restart. The combined
delegation/toast writer now uses marked version5 and reads both historical v4
layouts without guessing attribution; see [the integration verification](notification-persistence-integration-2026-11-18.md).
Versions
2 and 3 remain readable, and version 1 retains its existing legacy-only read
rule. These earlier formats represent owner-origin notifications and receive
the owner as their posting identity. This prevents a newly granted delegate
from cancelling an older owner-origin retained notification.

## Android evidence

The primary source is AOSP `android-16.0.0_r4`, frameworks/base commit
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:

- [NotificationManagerService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/notification/NotificationManagerService.java):
  `setNotificationDelegate`, `getNotificationDelegate`, `canNotifyAsPackage`,
  `resolveNotificationUid`, `cancelNotificationInternal`,
  `getAppActiveNotifications`, and delegated channel reads.
- [PreferencesHelper](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/notification/PreferencesHelper.java):
  delegate package/UID/enabled preferences and package-removal handling.
- [NotificationManager](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/NotificationManager.java):
  public delegation, post/cancel attribution, and active notification methods.

The exact source URLs, Git blob IDs and SHA-256 values are retained in
[`aosp-source-manifest.json`](../../tests/notification-delegation/reference/aosp-source-manifest.json).

Five original SDK-only APKs were built and signed: owner, delegate, unrelated
caller, and two genuine shared-UID packages. They were run on an owned Android
16 emulator with this observed profile:

```
Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys
API 36; sdk_full 36.0; x86_64
```

All 80 queued ADB steps ran, yielding 52 fixture observations. The result parser
reported no missing frames, transport failures, or polling deadlines. The raw
commands/results, every notification polling sample, APK hashes and device
properties are preserved in
[`android-api36.json`](../../tests/notification-delegation/reference/android-api36.json).
The SDK input and generated APK hashes are in the
[`fixture receipt`](../../tests/notification-delegation/reference/sdk-fixture-receipt.json).
The earlier standalone ADB attempt failed before installation and is not part
of this Android evidence.

The reference confirms the operation-specific shared-UID table, grant/revoke,
channel reads, original-poster cancellation, owner/delegate visibility,
permission asymmetry, unchanged preference for an unknown delegate, process-stop
persistence, delegate reinstall requiring regrant, and source reinstall clearing
the preference. The owner UID was 10150, its delegate changed from 10151 to 10154
after reinstall, and the shared pair both had UID 10153.

Visibility probes poll for the requested notification for at most 6500 ms.
Absence probes retain an observed absence window of at least 2500 ms and retain
timing uncertainty; an immediate empty array is never treated as a delivery
contract. The run also contains Android-generated `Aggregate_SilentSection`
summary records. The delegation comparison concerns app-origin records and
their owner/poster identity; equal total active-record counts are not claimed.

## Runtime verification

| Verification | Observed result | What it establishes |
| --- | --- | --- |
| Full production Java compilation against framework/coremod inputs | Pass, 326 source files | Production sources compile together |
| Focused host fixture, protected profile | Pass, 132 checks | Actual service/caller/permission/persistence logic with controlled identity and metadata boundaries |
| Focused host fixture, legacy profile | Pass, 121 checks | Package authentication without treating UID10000 collisions as shared ownership |
| Same fixture with the base notification service as negative control | Fails at missing source-ownership enforcement | Assertions detect the former default delegation path |
| Existing notification channel/group host regression | Pass, 88 checks | Existing channel/group lifecycle and publication behavior remains covered |
| Complete services-test DEX build | Pass | Fixture compiles and DEX generation succeeds |
| Owned Android 16 reference | 80/80 commands, 52 observations | Real public APIs across distinct applications and a real shared-UID pair |
| Windows ART/Parcel execution and actual toast delivery | Pending | No Windows execution or delivery qualification is claimed |

`tools/test_notification_delegation.py` recompiles production notification code
and runs `NotificationDelegationTest` through a separate fixture class loader.
Its native adapter supplies Binder PID/UID and filesystem primitives; it does
not replace Java authorization or grant logic. Constructed parsed manifests and
process attachments are explicit fixture boundaries. Permission definitions are
supplied only on the host, whose Android asset JNI is unavailable. The signed
continuity case uses real signed APK bytes and the production APK verifier, but
does not constitute an end-to-end Installer update test.

The Windows track runs the same policy fixture using ART and generated Binder
parcels. It additionally checks the version-4 retained poster and version-3
owner-only migration through real Parcel storage. Its toast sink is explicitly
test-only. The gate is prepared; it has not run in this session.

## Reproduction

Build the SDK corpus with official API 36 SDK inputs:

```sh
python tools/build_notification_delegation_fixture.py \
  --android-jar "$SDK/android.jar" --build-tools "$BUILD_TOOLS" \
  --java-home "$JAVA_HOME" --output "$OUTPUT/apks"
```

Run it directly on an explicitly selected owned Android device, or export the
same fixed plan for the process that owns that device's ADB connection:

```sh
python tools/observe_notification_delegation_android.py \
  --adb "$ADB" --serial "$SERIAL" --apks "$OUTPUT/apks" \
  --output "$OUTPUT/android.json"
python tools/notification_delegation_queue.py export \
  --apks "$OUTPUT/apks" --output "$OUTPUT/request.json"
python tools/notification_delegation_queue.py parse \
  --request "$OUTPUT/request.json" --result "$OUTPUT/raw-controller-result.json" \
  --output "$OUTPUT/android.json"
```

The queue changes only the five fixture packages. Only their initial uninstalls
may fail. The parser verifies each executed argument array against the request
and preserves all raw output.

Run the focused host diagnostic against a fresh production app-classes build:

```sh
python tools/test_notification_delegation.py --java-home "$JAVA_HOME" \
  --framework-classes "$FRAMEWORK_CLASSES" --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" --apksig "$APKSIG_JAR" \
  --signed-apk "$OUTPUT/apks/delegate.apk"
```

Use `--service-source-ref 473beec761bd3005a23a8f746a6ba9216343f39d` for the
expected failing negative control. Framework CI runs the focused host fixture,
the existing group regression, and an official-SDK compilation of the receiver.
Windows track 94 invokes `gesher.services.NotificationDelegationTest`.

## Remaining scope and dependencies

F32 remains open. Snoozing, DND, bubbles, conversations, Android automatic
grouping and their full presentation/lifecycle contracts need separate work.
The reference corpus does not qualify real Windows toast presentation, native
notification-center retention, listener delivery, process-distributed Gesher
identity, signer rotation installation, or long-duration concurrency.

Current main's `CallerIdentity` authenticates an attached remote process and,
in protected mode, its UID. The existing local service/host control plane stays
trusted. This increment does not establish app isolation for every execution
profile or change that boundary. F07 identity work is already owned by open
PR #70 and is not duplicated here. `PackageUids` currently allocates distinct
protected-mode UIDs per package and does not implement manifest shared-UID
installation. The shared-UID service branch is tested with constructed
authenticated identities and independently observed on Android; end-to-end
Gesher shared-UID package allocation remains an identity/install dependency.

The Windows gate must run on the reviewed build, and independent QA must review
the final commit before this increment is proposed for merge. This document is
evidence for the bounded delegation change, not a completed issue checkbox.
