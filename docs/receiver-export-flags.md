# Dynamic receiver export flags

Addresses #56 F22. Receiver registration follows Android 16 r4's
`DYNAMIC_RECEIVER_EXPLICIT_EXPORT_REQUIRED` change (161145287), enabled since
target SDK 34. Android's UID decision requires the change to be enabled for
every package sharing that UID; the lowest target SDK therefore governs this
change when no runtime compatibility overrides exist.
An empty UID uses the enabled default of this AOSP change.

The actual `IActivityManager.registerReceiverWithFeature` path validates caller
identity before applying the export policy and before allocating receiver state.

| Registration | Contract |
| --- | --- |
| Both exported and not-exported flags | `IllegalArgumentException` |
| SDK 34+ UID, non-system action, no explicit export flag | `SecurityException` |
| SDK 34+ UID, visible-to-instant-apps plus not-exported | `IllegalArgumentException` |
| Exclusively protected system actions | Explicit export flag is optional |
| Filter with no actions | Explicit export flag is optional |
| Null receiver querying sticky broadcasts | Explicit export flag is optional |
| Legacy UID without not-exported | Exported by default |

A filter mixing protected and ordinary actions is not exempt. Android starts
`onlyProtectedBroadcasts` at true and clears it only for an unprotected action, so
a filter with no actions is exempt, exported unless not-exported is requested, and
matches no broadcast. media3 `RequirementsWatcher` registers such a filter without
flags for its default network requirement on API 24+. An unreadable framework
catalog cannot establish the protected-action exemption. The existing sender
policy still denies unknown protected actions when catalog loading fails.

Protected broadcasts require system authority in every runtime compatibility
mode. The registration exemption must not permit other applications to impersonate
system broadcasts when the optional GMS integration is disabled.

Non-exported dynamic receivers admit senders sharing their UID. A registered
sender permission remains a separate check, including for broadcasts from the
receiver's own package, and uses the sender UID's effective grants. Process death
or package replacement during registration is rechecked before receiver state is
published, under the same lock used for death cleanup.

Registration, exported/non-exported delivery and unregistration continue to use
the existing controller and application thread. No alternate broadcast transport,
synthetic success or per-application exception is added. The registration policy
performs bounded scans of installed UID members and filter actions and creates no
background task, retained cache or per-registration timer.

## Validation

`tools/test_receiver_exports.py` executes the production PM/ATM/AM and broadcast
controller through the actual public service entry point, in both compatibility
modes. The fixture replaces parsed APK construction, framework resource definitions
and declared native initialization/identity boundaries only. It checks target-SDK
and shared-UID behavior, exception types, protected/mixed/empty filters, sticky
queries, rejected-registration state, actual receiver delivery and cleanup. Its
negative-control option compiles a previous controller against the same fixture.
It also covers shared-UID delivery and permission grants, own-package sender
permission enforcement, mismatched caller identities, and a host-only late-death
interleaving between authentication and registration publication.

Windows track 94 runs the fixture through generated Binder Parcel dispatch within
one process. Host checks do not establish real cross-process Binder, ART execution
or a fresh full platform build; the latest commit's CI remains required.

## Primary sources

- [Android 16 r4 BroadcastController](https://github.com/aosp-mirror/platform_frameworks_base/blob/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/am/BroadcastController.java): change declaration and receiver registration flag policy.
- [Android 16 r4 PlatformCompat](https://github.com/aosp-mirror/platform_frameworks_base/blob/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/compat/PlatformCompat.java): `isChangeEnabledByUidInternal` requires every UID member to enable the change.
- [Framework manifest](https://github.com/aosp-mirror/platform_frameworks_base/blob/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/res/AndroidManifest.xml): actual protected broadcast definitions.
- [BroadcastSkipPolicy](https://github.com/aosp-mirror/platform_frameworks_base/blob/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/am/BroadcastSkipPolicy.java) and [ActivityManager](https://github.com/aosp-mirror/platform_frameworks_base/blob/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/ActivityManager.java): independent dynamic receiver permission/export checks and shared-UID access.
- [CompatChange](https://github.com/aosp-mirror/platform_frameworks_base/blob/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/compat/CompatChange.java): enabled default for non-disabled changes.
