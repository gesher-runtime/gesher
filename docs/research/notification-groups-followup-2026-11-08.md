# Notification channel groups and publication — 2026-11-08

This follow-up audits PR #8 against the repository's pinned
`android-16.0.0_r4` framework. It supplies real group/channel state and repairs
nearby publication behavior; it does not establish compatibility with every
Android application or implement all of NotificationManagerService.

## Missing public functionality

`NotificationManagerService` inherited no-op/null implementations of
`createNotificationChannelGroups`, `getNotificationChannelGroup`,
`getNotificationChannelGroups`, `getNotificationChannelGroupsWithoutChannels`
and `deleteNotificationChannelGroup`. A valid public
`NotificationManager.createNotificationChannelGroup()` returned normally but the
same manager could not retrieve the group. Applications relying on that
read-after-create contract could lose their notification configuration or fail
when using the missing group. The Android 16 manager also reconstructs groups
from cached channel lists and group headers, so implementing only the older
single-group query would leave the public cached path broken.

The service now implements those operations using per-package group metadata and
actual channel membership. Empty groups are returned, ungrouped channels do not
invent an extra public group, member channels survive group renaming, and header
queries omit members. The real framework `NotificationChannelGroupsHelper`
performs association/filtering. New app groups cannot supply blocked policy,
user-lock fields or embedded channels. Existing group policy survives metadata
updates. Group count is bounded at 6000; channel count, including retained
recent deletion records, is bounded at 5000.

The channel path now validates group existence and conversation-parent
existence, preserves user policy and original channel group on app updates,
allows the normal unlocked importance downgrade, and prevents app creation of
the reserved default channel. Ingress and query results copy channel objects,
vibration arrays, composed vibration segments and vendor vibration bundles;
mutating one returned value does not mutate server state. Both channel and group
manager caches are invalidated for their relevant changes.

Sources:

- [NotificationManager: public group APIs and both query caches](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/NotificationManager.java)
- [NotificationChannelGroupsHelper: group/member query contracts](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/notification/NotificationChannelGroupsHelper.java)
- [PreferencesHelper: create/update rules, limits and deletion](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/notification/PreferencesHelper.java)
- [NotificationChannel: copying, user policy and XML](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/NotificationChannel.java)
- [NotificationChannelGroup: metadata, member copying and XML](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/NotificationChannelGroup.java)
- [VibrationEffect: mutable composed/vendor payload ownership](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/os/VibrationEffect.java)

## Durable policy and real deletion

Each package's policy is stored in
`$ANDROID_DATA/system/notification-channels/<package>.xml`, using the framework's
typed XML serializers and `AtomicFile`. The record includes format version,
package name and UID; malformed ownership/version/count data is not loaded.
Policy loads lazily and survives a new service instance. Channel deletion keeps
a deleted-channel record so an explicit channel re-creation preserves existing
settings; deleted records older than Android's 30-day retention boundary are
excluded when loading.

A complete candidate is written and synced before replacing live maps. An I/O
failure leaves the old live state intact and reports failure. Because
`AtomicFile.finishWrite()` logs a failed final rename rather than throwing, the
service additionally checks that the `.new` candidate was consumed. Unchanged
requests avoid unnecessary rewrites. Uninstall verifies that the base file,
`.new` and `.bak` remnants are gone before clearing live policy, preventing a
failed deletion from restoring old settings on same-name reinstallation.

Deleting a group deletes all its member channels, removes their active
notifications, cancels their host toasts and removes retained click records.
Both deletion and publication use the existing publication lock. A post captured
before channel/group deletion cannot publish after deletion or after recreation
of the same IDs. An unchanged re-creation retains the channel object, so an
idempotent app setup call does not discard a valid pending post.

`NotificationStore` format 3 records the notification's channel ID, allowing
selective cancellation of retained Windows toast records after a service
restart. Version 2 remains readable; version 1 retains its previous protected-mode
restriction. **Version 1/2 records have no channel identity, so they cannot be
selectively removed by channel/group.** They remain readable for existing click
and explicit cancellation behavior; a later new-format save supplies channel
identity. No channel identity is fabricated during migration.

Sources:

- [AtomicFile: write, commit, recovery and deletion behavior](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/util/AtomicFile.java)
- [AOSP NotificationManagerService: group preflight and channel removal](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/notification/NotificationManagerService.java)

## Replaced fabricated publication behavior

Previously, any absent channel produced a temporary default-importance channel
and the notification proceeded. Now a missing named channel is rejected from
publication. An actual app targeting below API 26 receives the real reserved
default channel, with `IMPORTANCE_UNSPECIFIED` policy; null/default IDs resolve to
that channel. Arbitrary missing names do not fall back to it. Upgrading the app
target to API 26 or newer removes its legacy default channel.

Legacy notification importance is calculated per notification from its priority,
valid sound/vibration request and applicable user locks, including the quiet and
noisy bounds used by NotificationRecord. The calculated value is used for toast
behavior and ranking; it never overwrites the stored unspecified importance.

Ordinary notifications on an importance-NONE channel are now filtered before
active-record/listener publication, rather than being retained as active while
only their toast is hidden. The pinned media-notification exception remains.
Privileged channel updates now apply actual user policy, preserve its lock fields
and cancel existing notifications when the channel is blocked, using
`REASON_CHANNEL_BANNED`. App channel creation/metadata updates remain distinct
from that privileged user-setting path.

Sources:

- [NMS: missing-channel rejection, blocked-record checks and privileged channel updates](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/notification/NotificationManagerService.java)
- [NotificationRecord: pre-channel detection and initial importance](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/notification/NotificationRecord.java)

## Foreground-service and job ownership

Group/channel deletion preflights every member before changing state. It refuses
channels associated with an active foreground service or user-initiated job.
Those guards use trusted service/job state, not an app-provided notification flag.
Foreground service ID/channel replacement and stop/process removal retire the
old association. Job associations and end-notification policies are covered in
[the job notification follow-up](job-notification-followup-2026-11-08.md).

Trusted association changes share the publication lock. Listener callbacks are
queued during that outer operation and delivered after the lock is released;
callbacks can therefore perform channel operations on another thread without
waiting on the caller's held publication lock. Group deletion still notifies
ordinary notification removal after its mutation fence.

## Executed evidence

The local host diagnostic is reproducible with the repository's verified
framework/coremod compilation inputs and app compilation jar:

```sh
python3 tools/test_notification_channel_groups.py \
  --java-home /path/to/jdk21 \
  --framework-classes /path/to/framework-classes.zip \
  --core-classes /path/to/coremod \
  --app-classes out/audit-baseline/gesher-app-classes.jar
```

The script recompiles the five production notification/service/job source files
against the real framework classes. It executes the real `NotificationManager`,
channel/group values, cache code, framework XML, `AtomicFile` and service methods.
Its separate JNI fixture supplies local caller identity, JDK array allocation,
monotonic time and real host filesystem primitives. It uses text typed XML
because ART's binary-XML native helpers are unavailable on this JVM. Toast calls
are observed through the service's existing injected sink.

**Final local result: 88 assertions passed.** Coverage includes:

- Public manager create/query/delete and cached membership/rename/deletion.
- Real channel/group state, non-null list contracts, bounds, validation and
  input/output isolation, including mutable vibration effects and retained invalid
  non-null patterns whose vibration effect is null.
- App/user policy separation and eight legacy-importance cases.
- Actual XML restart recovery, real failed file creation, uncommitted-state
  isolation and uninstall/cache cleanup.
- Actual production posting: missing and ordinary blocked channels do not
  publish; a privileged block removes an already active record; an app cannot
  reset the user lock; explicit unblocking restores publication.
- Deterministic paused-post races: unchanged create accepts the valid post,
  while delete/recreate rejects its stale predecessor.

The same script with
`--source-ref 3843241968edfd124d3b9596c0e6381f45f5c381` is a negative control. It
compiles the starting PR's production files and fails at the public manager
read-after-create assertion: the newly created group is missing. The negative
control does not fail due to a missing test-only helper.

A second narrow negative control ran the invalid-pattern regression against the
pre-correction copy helper. It failed at the service create/query assertion because
copying the null effect discarded the preserved pattern. The corrected helper
clones the original pattern and sets a copied effect only when non-null; all four
new ingress/output/restart assertions pass. Ungrouped notifications also skip
group lookup, so null membership cannot alias a real group ID spelled `"null"`.

## Full-runtime fixtures and limits

`gesher.services.NotificationChannelGroupsTest <devsvc.apk>` adds real generated
AIDL proxy/stub Parcel round trips, parsed package target/ownership, legacy default
resource initialization, native retained-store format 2/3 reads, real store
cleanup, listener dispatch outside the lock, and foreground/job lifecycle guards.
It requires `GESHER_BINDER_UID=1000`, `android_runtime` and framework resources.
Its success marker is `NotificationChannelGroupsTest all passed:`.
`NotificationSecurityTest` and `NotificationRetirementTest` now register genuine
named channels so their permission/publication assertions do not depend on the
removed missing-channel fallback. `JobNotificationLifecycleTest` covers the
separate job policy behavior.

The complete application and all service fixtures compiled and dexed against the
verified framework and Android core module in this audit. **The Windows/ART
fixtures have not executed locally.** Host results do not establish real Binder
marshalling, translated ARM64 execution, Windows toast delivery or native Parcel
persistence correctness; those remain full-runtime CI gates. The host execution
uses the current cached manager branch and direct service queries, not a separate
runtime with the cache feature flag disabled.

This work does not add Android's full notification assistant/ranking pipeline,
all privileged policy administration, telecom-validated call exemptions, cloud
backup/migration or additional Android users. Existing version 1/2 retained
notification limitations are described above. Full user/background-restriction
job stop-reason plumbing is also outside the implemented normal completion
policy; see the job report. These limits must not be represented as completed
Android backend support.
