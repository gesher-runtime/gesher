# Managed Activity caller identity and historical URI permissions

This follow-up implements the Android 16 `ComponentCaller` contract for activities managed by Gesher's activity task manager. Previously, `IActivityClientController.Default.checkActivityCallerContentUriPermission` returned integer zero, which is `PackageManager.PERMISSION_GRANTED`, without examining the activity, delivery, URI or caller. An application that used the new API to validate an incoming content URI could therefore receive an invented grant. Caller identity methods also lacked authoritative per-delivery state.

This is a real historical capability implementation over Gesher's existing installed-provider, permission and explicit-grant policy. It does not manufacture URI grants, imply that every Android launch entry point is implemented, or replace the missing automatic Intent-grant and persistence backends.

## Implemented behavior

Each managed activity retains an initial caller token and a map of immutable delivery snapshots. A new Intent or result receives a distinct token. The initial token is passed through the actual `LaunchActivityItem` constructor; new Intent tokens use the three-argument `ReferrerIntent` constructor; results use the five-argument `ResultInfo` constructor, including a result whose data is null. These are the fields consumed by the pinned `ActivityThread` and `Activity` implementations, so `getInitialCaller`, callback `getCurrentCaller` and `setIntent(Intent, ComponentCaller)` refer to the appropriate delivery. The initial token survives the framework's local activity recreation. Removing the record or replacing its installed target generation makes its historical tokens unavailable.

The snapshot records the authenticated source UID/package and whether sharing was requested. Ordinary starts authenticate the attached application thread and process. PendingIntent starts use the verified creator and exact creator-code generation, even when a different package holds and sends the capability. Server-generated results use the actual trusted control-plane origin, not the identity of the app that originally requested a dialog. Result publication from an app requires that app's current owning process; a source record removed by process death cannot publish a fresh result afterward.

The captured URI set includes `Intent.data`, a single `EXTRA_STREAM` URI or an ArrayList, ClipData item URIs and nested item Intents. Read and write are evaluated separately against the real installed provider ownership, declared normal/runtime/path permissions and explicit exact/prefix grants. Delivered-but-inaccessible URIs remain distinguishable from URIs that were never delivered. Later grants, revocation or Intent mutation do not rewrite a historical snapshot. Query strings and fragments retain their exact URI identity.

Queries first perform Android's argument/full-current-access checks for the querying principal, before looking up the activity and caller token. Thus an unauthorized querying app receives `SecurityException` even for an unknown activity; an authorized query of an unknown/retired token is denied; an authorized query of a URI absent from a known delivery throws `IllegalArgumentException`. Negative UIDs, non-content URIs and modes with no read/write bit are invalid. Unrelated flag bits do not invalidate an otherwise valid full permission check. Prefix flags affect the query identity, and a nonpersistable explicit grant cannot satisfy a persistable-demand check; direct provider permissions retain their Android meaning.

Identity is exposed for explicit sharing, the authenticated same package and captured/current UID, or the trusted control plane. Gesher's legacy assignment of UID 10000 to unrelated packages is not treated as shared application ownership. A same-name reinstall under a different UID cannot recover a hidden historical identity. General Android shared-UID installation and platform-signing exceptions remain outside the available backend.

PendingIntent creation stores the share-identity option and sending applies the pinned normalized option merge. Both creation and sending can enable sharing; `FLAG_UPDATE_CURRENT` does not replace the creation option. The implementation follows the complete `SafeActivityOptions` path: both options are reserialized, and the serializer omits false values. Consequently, even an explicitly supplied raw false bundle does not erase a true creation value after that normalization. This is covered against the actual framework option classes rather than inferred from a raw `Bundle.putAll` alone.

## Publication and lifecycle guards

Authentication must still be valid when a request is accepted. The ordinary sender PID and an IntentSender holder's activity/process are checked through this boundary. Creator/package/capability generation remains checked afterward; mere sender exit does not cancel an already accepted queued launch.

This separation also repairs a concrete `CLEAR_TOP` defect. The accepted operation may itself finish a separate holder activity above the target. Rechecking that holder's unfinished state afterward previously returned a delivered-success result while dropping the new-Intent callback. The accepted delivery now retains creator and target generation checks without depending on the activity it just finished. The holder guard is still checked at the verified-creator entry, before external-handler branches, and again at managed publication. No task-affinity or general `NEW_TASK` reuse policy was added.

An activity record now retains the exact target package object resolved alongside its `ActivityInfo`; it does not re-read a potentially replaced package inside its constructor. Package permission capture occurs under the package monitor and completes before activity publication or Binder callbacks. Snapshot-map operations are local, and callbacks remain outside those monitors. The existing delegated-retirement and activity lifecycle gates remain in the normal suite. The new host fixture tests recipient-generation invalidation but does not simulate every publication interleaving; the source lifetime/lock-order review is separate evidence.

## Verification and its boundaries

| Check | Result at this follow-up | What it establishes |
|---|---|---|
| `tools/test_component_callers.py` | **116 checks passed**, independently rerun | Actual framework `ComponentCaller`/`ActivityClient`, production controller/ATM queries and URI/permission policy over constructed real parsed manifests and process/activity state |
| Prior-controller negative control, `--controller-source-ref 223d4b3b6a4947bd3c1b3c679d95fc944b4d956a` | **Fails** at `Sender write permission invented` | The regression detects the old inherited zero/granted answer |
| Components and peer APK build | **Both passed** actual aapt2, Android-core javac, D8 and zipalign | New manifest components and all callback/provider code compile against the pinned framework/core inputs |
| Production/controller fixture Android-core build | **Passed** target-17 javac and D8; root's combined application and all service fixtures also passed | Compile/integration evidence over the actual pinned core/framework inputs, not execution of Android services |
| PowerShell helper grammar | **Passed** tree-sitter parsing | Syntax evidence only; PowerShell and Windows were not executed locally |
| Actual ART/Binder contract and launched cross-app lifecycle | **Registered as mandatory Windows checks; execution pending for this revision** | Runtime proof must come from CI, not from the host run or compilation |

The host uses actual `PackageImpl`/parsed provider and permission values, constructed without APK parsing or unrelated server/native initialization. Its small native adapters provide controlled Binder PID/UID, log output, system-property/array/time boundaries; they do not implement permission or caller policy. The host explicitly logs that native framework resource metadata cannot load through `ApkAssets.nativeLoad`. The permission cases use their real constructed declarations, not a claim that all Android framework permission metadata was parsed. The host does not execute ART, Binder transport, real `ActivityThread` callbacks or provider file I/O. Its assertion count includes checks that the controlled identity was installed correctly.

The shared `ComponentCallerFixture` additionally supports a real strict StandIn/generated AIDL proxy path in `ComponentCallerContractTest`. Windows invokes it through `ServiceFixtureRunner`, which creates a writable `ApplicationSharedMemory` region before framework cache invalidation. This fixture still constructs its manifests/process/activity records and is not described as a launched-app test.

The separate real-app fixture uses the existing components.apk and peer.apk build artifacts and their new declared activities/provider. It starts an actual isolated device, then exercises:

- Shared initial identity and denied private-provider URIs from data, streams and nested ClipData.
- A later ordinary new Intent after a real exact read grant, with read/write separation and retention of the initial denial.
- Grant revocation without rewriting the previous snapshot, and actual activity recreation retaining the initial and saved delivery tokens.
- A components-created PendingIntent sent by a separate peer holder above the target, so `CLEAR_TOP` finishes that holder before the callback. Creator-only sharing identifies components and retains its revoked URI denial despite the holder owning the provider.
- A second capability with sender-only sharing through the actual IntentSender Activity API, again preserving the creator's authority.
- Distinct result tokens with URI data and with null data, followed by a provider-owner result back to components.
- Actual `ContentResolver.openInputStream`/`ParcelFileDescriptor` transfer and file bytes after an explicit read grant, with write/neighbor queries denied by the recipient's current access.

The helper `tests/windows/component-caller-contracts.ps1` owns a unique data/log tree and disables only unrelated Start-menu/URL registration through the existing `GESHER_SHORTCUTS=0` option. It captures real process handles for the state-listed binderd, servicemanager and system_server, checks each creation FILETIME against the owned device state, and waits on those retained handles after `Launcher shutdown`; PID reuse cannot substitute another process. The source app's exit is observed by the bounded Launcher command. App PID generations are not inferred from logs, and the helper does not claim separate retained handles for every app. Environment restoration and data cleanup remain attempted even if a diagnostic log read fails. A shutdown failure is a test failure and prevents deletion of a possibly live owned device tree.

The runtime markers are `ComponentCallerContractTest all passed:`, `ComponentCallerLifecyclePeer all passed:`, `ComponentCallerLifecycleTest all passed:` and the enclosing `ComponentCallerBootstrapTest all passed: real Binder and cross-app callbacks; owned device stopped`. No marker is produced as a replacement for a failed operation. The modified real manifest inventories are seven activities for components, and three activities/two providers for peer; the existing Windows inventory assertions are updated exactly.

### Reproduce the host evidence

```sh
python3 tools/test_component_callers.py \
  --java-home "$JDK_HOME" \
  --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$ANDROID_CORE_CLASSES" \
  --app-classes "$GESHER_APP_CLASSES" \
  --apksig "$APKSIG_JAR"
```

Add `--controller-source-ref 223d4b3b6a4947bd3c1b3c679d95fc944b4d956a` for the failing old-controller witness. Build the two APKs with their existing `tests/apps/{components,peer}/build.sh` scripts and the same pinned framework-resource/framework-class/core-module inputs as CI. The mandatory Windows helper runs after the existing graphics/font/service prerequisites; it does not substitute the host fixture for a missing runtime artifact.

## Explicit remaining boundaries

- Standalone `GesherAppDriver` activities have no managed ActivityRecord or authoritative launch delivery and do not acquire an invented caller history.
- Automatic Intent/ClipData URI grant issuance, activity-owned grant revocation, persistable grants and durable task/caller history are not implemented by this patch. The real-app fixture uses explicit actual grants, and historical snapshots are not permissions to open a URI.
- Arbitrary provider-side dynamic authorization (`forceUriPermissions` and custom provider policy beyond the existing evaluated backend), additional Android users, general shared UID/platform signing and absent launch APIs such as `startActivityAsCaller` remain explicit gaps. Unsupported user/path checks are not relaxed into authorization.
- Existing chooser/task-affinity/`NEW_TASK` reuse and process-death recreation behavior are not completed here. The supported lifetime is a managed record's actual lifetime; retirement makes its tokens unavailable.

## Pinned primary sources

All comparisons use Android `android-16.0.0_r4`:

- [ComponentCaller](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ComponentCaller.java), [ActivityClient](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ActivityClient.java), and [Activity](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/Activity.java): public query and callback behavior.
- [ActivityClientController](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityClientController.java), [ActivityCallerState](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityCallerState.java), and [ActivityRecord](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityRecord.java): per-delivery state, identity visibility, URI collection and query ordering.
- [UriGrantsManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/uri/UriGrantsManagerService.java): full permission checks and advanced mode/strength semantics.
- [PendingIntentRecord](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/am/PendingIntentRecord.java), [SafeActivityOptions](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/SafeActivityOptions.java), and [ActivityOptions](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ActivityOptions.java): creator attribution and normalized option merging.
- [ActivityThread](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ActivityThread.java), [ReferrerIntent](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/content/ReferrerIntent.java), and [ResultInfo](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ResultInfo.java): actual caller token parcel/delivery paths, including null result data.
