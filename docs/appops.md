# AppOps modes and data delivery

Gesher's AppOps Binder and server consumers use the same `PackageManagerService.appOps()`
instance. `Services.create` registers the permission checker in both server and
in-process modes. Runtime AppOps are derived from the UID's permission state:
a binary permission grant can still produce `PERMISSION_SOFT_DENIED` when its
compatibility or restriction flags deny the operation, or its foreground access
is unavailable. A permission with the Android `appop` protection flag uses
the operation's explicit mode and falls back to the permission for `MODE_DEFAULT`.

The contract reference is Android 16 r4, frameworks/base commit
[`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/):

- [AppOpsService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/appop/AppOpsService.java)
  defines caller authority, switch operations, UID precedence, default modes,
  proxy accounting, client-token lifetimes and callbacks.
- [AppOpsManager](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/android/app/AppOpsManager.java)
  supplies the actual operation defaults, permission mappings and switch table.
- [AppOpsUidStateTrackerImpl](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/appop/AppOpsUidStateTrackerImpl.java)
  defines foreground capability checks and state settling. Gesher uses the
  platform default intervals (5 seconds for top/foreground-service downgrades,
  1 second for other downgrades); process removal commits immediately.
- [PermissionManagerService.PermissionCheckerService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/pm/permission/PermissionManagerService.java)
  defines permission checks, registered attribution chains, data-delivery
  transactions and their completion.
- [AppOpService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/permission/java/com/android/server/permission/access/appop/AppOpService.kt)
  (`getUidModeFromPermissionState`, `evaluateModeFromPermissionFlags`,
  `setUidMode`, `setPackageMode`) makes runtime AppOps permission-derived and
  ignores independent runtime-op mode records.
- [PermissionFlags](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/permission/java/com/android/server/permission/access/permission/PermissionFlags.kt)
  (`isPermissionGranted`, `isAppOpGranted`) distinguishes a binary grant from
  restriction and `REVOKED_COMPAT` denial. [PermissionService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/permission/java/com/android/server/permission/access/permission/PermissionService.kt)
  defines flag updates, legacy grants, system-fixed grants and fuller permissions.
  Foreground/background relationships and restriction metadata come from the
  actual [platform manifest](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/res/AndroidManifest.xml).
- [AppIdPermissionPolicy](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/permission/java/com/android/server/permission/access/permission/AppIdPermissionPolicy.kt)
  (`evaluatePermissionState`) derives the minimum target SDK from the packages
  requesting that particular permission and excludes runtime-only permissions
  from automatic legacy grants.
  Its `onPackageRemoved`, `trimPermissionStates` and `setPermissionFlags` methods trim a shared
  UID's permissions when no surviving member requests them, and publish flag
  changes. `AppOpService.onPermissionFlagsChanged` / `onStateMutated` then notify
  UID mode listeners; `PermissionService.onStateMutated` schedules normal UID
  process retirement when a runtime grant disappears.
- [AccessCheckingService](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/permission/java/com/android/server/permission/access/AccessCheckingService.kt)
  (`mutateState`) publishes state and notifications independently of ordinary
  asynchronous persistence. [AccessPersistence](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/permission/java/com/android/server/permission/access/AccessPersistence.kt)
  coalesces pending snapshots, separates scheduling and file-write locks, and
  handles serialization failure inside the writer.

## Policy and ownership

For a non-runtime operation, an unset mode has `opToDefaultMode(op)`. Explicit `MODE_DEFAULT` is retained
when it differs from that operation's initial mode. A nondefault UID mode takes
precedence over the package mode. Aliases use `opToSwitch`, while reported
operation events retain the requested operation. A normal initial-allowed start
works with `startIfModeDefault=false`; accepting an explicit default is a separate
caller choice.

For each runtime permission, only its canonical `permissionToOpCode` operation
derives a mode from that permission. Switch aliases use that derived mode. A
revoked foreground permission yields `MODE_IGNORED`; a granted foreground
permission with a denied background permission yields `MODE_FOREGROUND`; granted
foreground and background permissions yield `MODE_ALLOWED`. Permissions with no
background counterpart need only the foreground grant. Coarse location uses the
platform's fuller fine-location grant when applicable. UID queries include these
computed nondefault modes, even when there is no independently stored mode.

`PermissionPolicy` retains UID permission decisions and API flags together.
`REVOKED_COMPAT` and soft restrictions can deny AppOps while leaving the binary
permission granted. Hard restrictions deny a modern runtime grant. A legacy
requester supplies an automatic grant only for a runtime permission without
runtime-only protection, and compatibility flags control that grant's AppOps
access. The minimum target SDK is computed separately for each permission from
its actual requesters. A legacy shared-UID member that does not request a
permission cannot turn another member's denial into a grant or supply its
legacy review flag. Shared packages observe one UID policy; retiring the last requesting
member clears its stored decisions and flags. Flag updates retain the existing
authenticated system authority requirement.

Runtime-only permissions start denied for legacy requesters, with no automatic
review or user-decision flags. Restriction exemptions do not grant them.
The normal permission API still accepts an explicit grant through an eligible
modern requester, and that grant is shared across the UID. Calls targeting a
legacy requester retain the platform's no-op runtime-grant behavior. AppOps UID
setters only change compatibility flags on an existing grant. As in
`PermissionFlags.toApiFlags` / `fromApiFlags`, the implicit flag is exposed as
`REVIEW_REQUIRED` only when a legacy grant exists; otherwise it is exposed as
`REVOKE_WHEN_REQUESTED`. Reads and flag updates use the stored legacy grant basis;
package publication and boot reconciliation update that basis from the actual
requesting packages.

### Package publication and stored flag transitions

`AppIdPermissionPolicy.onPackageAdded` evaluates the permission states requested
by the published package. Its `evaluatePermissionState` preserves a previous user
denial as `APP_OP_REVOKED` when adding `LEGACY_GRANTED`. It also handles the reverse
transition when requesters become modern. Gesher records the legacy grant basis
alongside the explicit runtime decision and API flags in its existing permission
snapshot. A later authorized compatibility clear therefore survives reads,
republication and restart; the old user-decision flags do not cause revocation to
be added again during a query.

The bounded ordinary-runtime transition rules are:

| Publication or lifecycle change | Prior state | Result |
| --- | --- | --- |
| An actual legacy requester is admitted | Modern denial with user flags | Binary legacy grant; compatibility revocation keeps raw AppOps ignored |
| An actual legacy requester is admitted | No previous grant or flags | Legacy grant with review required |
| An actual legacy requester is admitted | Explicit runtime grant | Retain the grant without inventing compatibility revocation |
| A legacy package is republished | A legitimate compatibility or review clear | Keep that clear and the stored legacy grant basis |
| Requesters become modern | Reviewed, compatible legacy grant | Convert it to an explicit runtime grant |
| Requesters become modern | Unreviewed legacy grant | Remove legacy/review state; ordinary unfixed permission remains denied |
| Requesters become modern | Compatibility-revoked legacy grant | Deny the runtime grant and clear obsolete compatibility state so permission can be requested normally |
| A legacy requester of a runtime-only permission is published | Previous grant or user flags | Remove grant/user state and retain restriction exemptions |
| A nonrequesting package is published | An unrelated permission decision | Preserve that permission state |
| A requester is removed | Another member still requests the permission | Retain the UID flag record, as in `trimPermissionStates`; a later requesting-package publication reevaluates it |
| The last requester is removed | Any permission decision or grant basis | Clear UID ownership and publish the normal revocation lifecycle |

The publication path snapshots UID permissions before replacing package metadata
and reconciles flags while holding the permission monitor. Permission readers
cannot observe the new metadata with old denial flags. Mode events capture the
current package generations in mutation order, then wait for the package or
installer publication lock before invoking clients. Normal UID cleanup also runs
after those locks are released and rechecks a newer grant or generation before
retiring a process. File writes remain asynchronous and use the existing coalesced
writer. Boot admission reconciles durable flags against installed metadata, including
when a package was published but its deferred permission write failed.

Android 16 ignores `setMode` for runtime operations. Its outer `setUidMode`
implementation first runs `updatePermissionRevokedCompat`: it updates compatibility
flags on **already granted** switched foreground/background permissions, then its
checking service ignores the independent runtime mode setter. Gesher follows that
sequence. `MODE_ALLOWED` never grants a missing permission or background capability,
and requested `MODE_ERRORED`/`MODE_DEFAULT` values become compatibility revocation;
the returned mode still follows the permission and fuller-permission rules.
Permission changes enqueue live-operation pause/resume and watcher updates after
publication. The runtime mode listener only enqueues work; it does not acquire
the AppOps operation monitor or invoke an application callback under the permission
or package monitor.

Mode writers require `MANAGE_APP_OPS_MODES`. Note/start/finish requests authenticate
the incoming UID and the installed package; delegated attribution follows the
platform's trusted-source rules. Statistics require their framework permission.
The runtime has no virtual-device implementation, so unknown devices receive
`MODE_IGNORED`. Authentication and operation validation still apply. Direct and
proxy note/start calls reject an unavailable device before history, observer
notification, asynchronous-note collection or client death registration.
PermissionChecker rejects an unavailable delivery before linking attribution
tokens. Invalid device IDs therefore cannot accumulate operation state.

Persisted modes include the full UID, package name and verified signer identity.
In-memory publication and observer notification do not wait for disk. The shared
`PolicyStateWriter` schedules immutable AppOps and permission snapshots after one
second of quiet, with a two-second scheduling deadline from the first mutation.
Each writer keeps only its latest pending snapshot; canceled tasks are removed
from the shared daemon executor queue. File serialization has a separate lock,
so an older in-flight write cannot overtake a newer write. Neither the package nor
the operation monitor is held for serialization or atomic commit.
One-time permission grants and their session decision flags are excluded from
durable snapshots; this does not implement or claim session-expiration scheduling.

Serialization failure is logged by the writer, leaves current policy intact, and
keeps the latest snapshot dirty for a later mutation or explicit flush. There is
no busy retry loop. `reloadNonHistoricalState` flushes AppOps modes and keeps
published memory authoritative, including on failure; Android 16's checking
service implements its `readState` entry as a no-op. Package retirement is
an explicit durability boundary: ownership removal is flushed outside the
package/operation monitors, and a failure prevents successful uninstall. Legacy
experimental AppOps files without signing
ownership are not used to confer grants. Uninstall retires ownership, and a
signing change cannot inherit old modes. Invalid input is not partially loaded.
The server-local generation-checked UID setter makes a desktop special-access
decision conditional on the same package still being installed.

## Lifetimes and callbacks

Removing the last permission-requesting shared-UID member changes both permission
checks and live operation state for surviving members. `forgetPackage` captures
the old permission/mode state, excludes the exact retiring APK generation from
the new membership, trims unrequested decisions and flags, and publishes the UID
mode transition. The intermediate membership matters for legacy grants too: the
APK remains in the package registry until the uninstall transaction commits, but
its manifest must no longer supply permissions to the surviving UID at this stage.
The generation marker uses weak ownership and is cleared at commit or rollback.

Runtime grant loss uses the existing `killUidForPermissionChange` / `processDied`
path, anchored to a current surviving package generation. It retires attached and
pending UID process generations while preserving ordinary activity/service recovery.
Package identity and the revocation are rechecked before further retirement, including
after client reentry. Application exits occur outside the package, permission and
AppOps monitors. Client Binder death then releases the actual operation starts and
permission data deliveries. A surviving member that still requests the permission
retains its grant, flags and running operation.

Mode publication and UID cleanup occur before the fallible uninstall durability
flush. A blocked or failed write cannot leave an authorized-looking live operation
under a revoked permission; failure still prevents successful uninstall. If the
transaction restores the package, its manifest becomes authoritative again and any
resulting mode transition is published. Deleted user runtime grants stay deleted;
an installed legacy manifest can again supply its automatic grant. Old process
generations and their completed deliveries are never resurrected by rollback.

Starts are nested per client Binder, operation, UID, package, attribution tag and
device. Finish only decrements that matching start. Empty clients release their
death registration. The permission checker records the accepted delivery and
finishes its actual participants; death of any participating attribution token
ends the delivery. Package replacement retires obsolete starts, observers and complete data deliveries,
including their token death registrations. A historical proxy event is not reported
as a currently running proxy.

Foreground state comes from authenticated task/process records and selected
foreground-service types, including while-in-use eligibility at service start.
A background process does not receive capabilities merely because an activity
record exists. Mode or foreground transitions pause and resume live starts.
Recent statistics contain actual access/reject times, durations, attribution and
proxy identity. They are live/recent accounting, not a new historical aggregation
or historical-export implementation.

Watch registrations deduplicate by Binder. Foreground callbacks respect
`WATCH_FOREGROUND_CHANGES`; reset operations notify the affected operations.
Callbacks and death unlinks run outside the operation monitor. UID settle timers
are canceled when superseded, and retired packages release their observers and
queued asynchronous notes.

## Validation

`AppOpsContractTest` exercises production PM/caller policy, AppOps and permission
checker methods with explicit constructed APK metadata. It covers Android mode
defaults, UID precedence, switch aliases, authority, generation-checked special
access, nesting, attribution and token death, watcher cleanup, foreground
settling/pause/resume, restrictions, data delivery, proxy accounting and signed
mode persistence. The expanded suite additionally covers runtime package/UID
setter semantics, permission revocation and compatibility flags, background
grants, fuller permissions, shared UID and legacy decisions, hard/soft restrictions,
system-fixed grants, computed UID queries, live flag-driven pause/resume,
unavailable-device resources and observer silence, blocked disk I/O, coalesced
writes, stale-write ordering, atomic failure recovery, unavailable state parents,
permission-flag persistence, and exclusion of one-time grants from restart state.
Shared-UID retirement regressions exercise the production uninstall stages, normal
UID process retirement and client death cleanup, surviving requesters, legacy grants,
blocked persistence, failed-uninstall rollback/recovery, and reentrant package
replacement and regrant. They check that stale generation callbacks are discarded
and that the final durability flush preserves a newer grant made during reentry.
Mixed-target regressions cover both requesting and nonrequesting legacy members,
runtime-only denial, explicit modern grants shared across the UID, implicit flag
conversion, compatibility flags, and retirement of a nonrequesting member without
changing an existing grant, live operation or watcher state.
Publication regressions exercise the transition table through `add` and
`packageInstalled`, including actual legacy requester admission, prior grant and
user denial, compatibility/review clearing, republication, requester removal,
modern upgrade, runtime-only reconciliation, boot recovery after a failed deferred
write, and a legitimate regrant before queued process cleanup. Existing rollback
and generation-reentry regressions remain required.

The Windows `framework-fixes` track runs the fixture through generated AppOps and
PermissionChecker Binder Parcel transports. Its required success marker includes
`489 checks; binder=true`. `tools/test_appops.py` runs the same service contracts on a host JDK
with the existing declared JNI identity/clock boundary. Its host-only setup also
supplies the pinned permission definitions, including their background and
restriction metadata, when framework assets are
unavailable, bypasses unrelated ICU initialization and disables the cross-process
cache nonce. An isolated broadcast transport boundary keeps package lifecycle tests local.
It does not replace permission decisions or operation methods.

The current regressions also cover missing app-op permission mappings, actual
accessor/intermediary/receiver observer roles, trusted and datasource chains,
explicit foreground-service exit, and service-removal accounting while another
activity retains the process. The service's earlier while-in-use eligibility is
cleared on an actual foreground exit; sticky-restart handling is unchanged.
Service removal schedules a UID-state refresh so the published ledger and
observers transition after settling without requiring a later query.

The 2026-11-18 repair composes the reviewed SAF/task/result changes and typechecks
288 production Java sources. All 489 host AppOps checks and the original
independent attribution and foreground-lifecycle assertions pass. The expanded
fixture rejects the original PR72 implementation at its missing-app-op mapping
assertion. These are host/source results; the new Windows Binder marker remains
an execution requirement, not a claim that this repaired tree ran on Windows.

Repair validation on 2026-11-17 compiled all 284 production Java sources, passed
434 AppOps assertions, passed the 85 UID checks in each compatibility mode and
both interrupted UID admission recovery cases, and built the complete services
test DEX. The three original independent contract probes, the independent
shared-UID lifecycle probe, both observations in the independent legacy
eligibility probe, and the independent denied-to-legacy publication probe also
pass without a change to their source. These builds use cached Android 16 APIs and historical
generated dependencies; they are not a fresh platform build. The unchanged CI
queue and source-identity suites also pass (32, 2 and 14 checks respectively).

A host result does not establish ART, remote-process Binder death, native Windows
process termination, Windows window
visibility, foreground-service process execution or hardware delivery. The
current commit's Windows CI remains required before automerge.
