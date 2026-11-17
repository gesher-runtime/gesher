# Durable package enabled state — 2026-11-17

Baseline: `6f5e23a598c10906e4518045bc8d73f5c0cf3a9f`. Scope: issue
[#56, F25](https://github.com/gesher-runtime/gesher/issues/56). This change covers
installed application/component enabled state, its publication and lifetime.
It does not close the aggregate audit or claim general package-manager parity.

## Defect and Android contract

Application entries in `setComponentEnabledSettings` were skipped. Component
overrides lived only in an `ApkPackage` instance; an APK update copied that map,
but a system-server restart lost it. The individual setters did not produce
package-change notifications or honor the process-restart behavior of their
flags. A disabled launcher alias, receiver, service or application could therefore
reappear after restart, and existing clients could keep stale metadata.

The expectations follow the project's pinned AOSP `android-16.0.0_r4` sources:

- [PackageManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/PackageManagerService.java),
  `setEnabledSettings`: application/component batches, duplicate validation,
  application-only states, permission checks and conflicting `DONT_KILL_APP` flags.
- [PackageUserStateUtils](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/pkg/PackageUserStateUtils.java),
  `isEnabled`: application state takes precedence over component state;
  `DEFAULT` uses the manifest and `DISABLED_UNTIL_USED` requires the matching flag.
- [PackageInfoUtils](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java),
  `updateApplicationInfo`: `enabled` and `enabledSetting` report different facts.
- [BroadcastHelper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/BroadcastHelper.java),
  `sendPackageChangedBroadcast`: component names, UID, `EXTRA_DONT_KILL_APP`,
  and registered-only delivery for component-only changes.

| Request | Result |
| --- | --- |
| Application `DEFAULT` | Restore manifest application state |
| Application `ENABLED` | Override a manifest-disabled application |
| Application `DISABLED` / `DISABLED_USER` | Hide its components from ordinary resolution, including explicitly enabled components |
| Application `DISABLED_UNTIL_USED` | Resolve only with the matching query flag and an enabled application manifest |
| Component `DEFAULT` | Restore that component's manifest state |
| Component `ENABLED` / `DISABLED` | Store an explicit override |
| Component `DISABLED_USER` / `DISABLED_UNTIL_USED` | Accept the valid enum but ignore this application-only transition, following AOSP |
| `MATCH_DISABLED_COMPONENTS` | Return component metadata, retaining separate component/application `enabled` values |
| Invalid or duplicate batch entry | Reject the whole batch before changes or side effects |
| Conflicting component `DONT_KILL_APP` flags within a package | Reject the whole batch |
| Nonexistent user | Setters return without changes and application getters report `DISABLED`; component getters retain AOSP's cross-user permission check and null-component ordering |

## Publication and installation lifetime

`PackageEnabledStore` owns one immutable map. A complete batch is validated first,
then written to a same-directory temporary file and synced. Atomic replacement of
`ANDROID_DATA/system/package-enabled-state` precedes one volatile in-memory
publication. Failed writes leave both old state and old file intact. There is no
delete-then-rename fallback when the filesystem cannot atomically replace the
snapshot. All calls use synchronous persistence, so `SYNCHRONOUS` is honored even
though ordinary calls also receive the stronger write-before-return behavior.

Each installed package has a UUID owner marker under
`system/package-enabled-owners`. APK updates retain the marker. Uninstall moves
it through the existing removal quarantine along with code/data: rollback restores
the marker; committed removal retires it before a future installation can inherit
an old snapshot row. A crash between that retirement and in-memory cleanup cannot
restore old overrides. Loading discards rows whose installation owner no longer
matches, and the next commit compacts them. Limits on package count, component
count and serialized bytes are enforced on both read and write; malformed data
fails explicitly rather than silently re-enabling applications.

Installation establishes the enabled-state owner before committing `packages.list`.
An owner I/O failure therefore leaves both the registry and the live package set
unchanged. Final package publication reuses the prepared record without repeating
owner I/O, and a registry-write failure restores the installer's previous entry.

The original five removal journal slots remain `item-0` through `item-4`. The new
marker has the stable slot `package-enabled-owner`. New independent removal paths
can use their own named slots without interpreting one another as positional
`item-5`. Old journals without this slot remain readable.

Installed records read the shared state snapshot. Metadata generation captures a
single state for every component family, and queries spanning multiple package
records share the package monitor with batch publication. Application metadata,
ordinary/explicit component queries, activity options, launcher selection, provider
lookup and authenticator discovery respect the resulting state. The existing
package-info cache is invalidated after successful publication.

`Services.createInProcess` selects a transient store, matching the existing
standalone account-isolation boundary. Standalone toggles and removal cannot write
the real device's enabled snapshot or installation markers. The real system server
retains the normal durable store and is its single writer.

## Notifications and existing processes

Successful changes enqueue one `ACTION_PACKAGE_CHANGED` per affected package, with
its actual UID and the changed application/component names. No-op/rejected/failed
writes enqueue nothing. Delivery and process callbacks run after the package
monitor is released. Component-only events have `FLAG_RECEIVER_REGISTERED_ONLY`;
application events do not. Delivery is queued per commit; Android's delayed
cross-call broadcast coalescing is not reproduced.

`DONT_KILL_APP` leaves existing processes running while new resolution immediately
uses the new policy. Without it, the activity manager retires captured live
process records. The record's recovery flag allows still-enabled sticky services
to recover; process-death and queued-restart paths reject newly disabled services.
An event for a replaced/retiring package record cannot kill its replacement.
Enabled-state changes do not add force-stop state. Queued component admission is
revalidated and canceled independently, and disabling an application cancels its
pending process reservations using their retained child identity.

Independent QA found a race in the first implementation: the generic process-ending
loop could accept a captured record, invoke an earlier application's exit callback,
then kill a later record's reused PID after a replacement attached. Enabled-state
retirement now rechecks each record immediately before dispatch. Native termination
uses the original retained child handle plus an expected creation identity; it never
reopens a PID. Identity verification and `TerminateProcess` use the same handle under
the child-registry lock, and the short native call remains under the activity-manager
monitor to exclude logical attachment supersession. No blocking wait is added.

The expected native identity is captured on `PendingProcess` immediately after
`ProcessStarter.start` returns, before publishing its PID or launching its exit
watcher. At that point the spawn's original handle has no exit-code consumer.
Attachment copies that identity instead of looking up ambient PID identity later.
A missing, zero, mismatched or consumed native identity fails closed; the captured
application-thread Binder can still receive `scheduleExit` outside the monitor.
The older force-stop/shutdown PID APIs are not changed by this scoped repair.

Caller authorization continues to use Binder identity and the existing
`CHANGE_COMPONENT_ENABLED_STATE` permission boundary. A claimed package string
does not grant authority. The local-service bypass requires the current process's
PID, rather than a shared UID. This does not repair the broader shared-UID and
default-dangerous-permission issues tracked by F07/S02.

## Verification and limits

### Maintainer lifecycle follow-up

The lifecycle correction starts from reviewed PR head
`e3a3c7cd5d62e97a6512b648e20650a579dc6e6b`, preserving its user-ordering,
install-owner preparation, registry rollback and metadata-fixture repairs.
The new `AGENTS.md` and `CONTRIBUTING.md` policy at main
`e6c16e84398f5101863b6da8eb42fb15a81d4de7` was read before completing this
correction. Scope verdict: ordinary Android compatibility. The changes apply to
installed components generally and preserve requested lifecycle and binding
outcomes; they introduce no application-specific exception, substituted input,
synthetic production capability or changed application workflow.

The contract evidence is pinned Android 16 `android-16.0.0_r4`:

- [BroadcastController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/BroadcastController.java)
  invokes disabled-component cleanup outside its process-kill condition.
- [ActivityManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/ActivityManagerService.java)
  rechecks current package/component settings and cleans activities, services,
  providers and queued receivers.
- [ActiveServices](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/ActiveServices.java)
  marks torn-down service connections dead;
  [LoadedApk](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/app/LoadedApk.java)
  dispatches that signal to `onBindingDied`, rather than the `onNullBinding`
  callback for a service returning null from `onBind`.
- [ContentProviderHelper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/ContentProviderHelper.java)
  removes disabled authorities and connections, invalidates unstable provider
  clients and retires nonpersistent stable dependent clients.
- [BroadcastRecord](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/BroadcastRecord.java)
  removes queued disabled manifest receiver targets.

PMS now invokes a server-local lifecycle observer registered by AMS. A broadcast
with `PACKAGE_CHANGED` extras is only a notification, never authority to stop a
component. Each controller checks the captured installation owner and current
effective enabled state. A later change to B cannot discard still-relevant A
cleanup. A requested process restart remains relevant if any of its non-KEEP
settings still matches; re-enabling A cannot cancel an independent B restart
from the same batch. Owner and setting checks repeat at each process retirement,
alongside the existing process-generation and retained-child-handle checks.

Process work now captures its installation, component name and component family.
The family matters because an activity-alias can share another component's name
while having a different manifest default. Admission rechecks enabled state both
before queuing and before attach-time execution. Rejected work invokes its
controller's cancellation path outside the task-manager monitor; it cannot leave
an old service reservation stranded until a delayed event, or resurrect it on
a later enable. Whole-application disable cancels unauthenticated process starts;
a late starter result is retired with its retained child identity, never a PID
reopen, and its watcher cannot remove a newer generation's work.

Service cleanup stops only the disabled service, cancels starts/restarts and
binding dispatch, reports binding death and releases client-capability entries.
`DONT_KILL_APP` also preserves the process when its last service or activity is
removed. Activity cleanup completes pending results with cancellation and uses
the ordinary finish/destroy lifecycle. Provider cleanup is scoped by authority
and installation, preserves sibling providers, wakes publication waiters and
rejects stale acquisitions, references and publications. Provider dependency
notifications use captured application-thread capabilities outside monitors.
Existing raw provider objects are not rewritten or represented as revoked OS
capabilities; cleanup affects the runtime's authority and connection lifecycle.

Queued manifest receivers are canceled. A dispatched receiver retains its
acknowledgement slot until finish, death or timeout, because this runtime uses the
application-thread Binder as the completion token for every receiver in a
process. Reusing that slot would let an old receiver finish a peer's delivery.
An already-disabled ordered receiver's late result/abort cannot change the
remaining chain.

`tools/test_package_enabled_lifecycle.py` executes real PM setters, AMS wiring,
service/receiver/provider controllers and `finishAttachApplication`, using only
constructed manifests, recording application threads, controlled event timing
and native boundaries. It covers both event/attach orders, same-process peers,
binding death, reentrant callbacks, application cancellation, pending activity
results, provider dependencies/waiters, ordered acknowledgement ownership,
mixed/stale/replacement events, family name collisions and retained pending-child
identity. The same unchanged fixture compiles against the e3a3c7 baseline and
fails behaviorally on both maintainer blockers: the running service remains
alive with KEEP, and disabled pre-attach work schedules create/start callbacks.
Normal framework CI runs it, and the Windows contract track builds/runs its ART
and PM-AIDL counterpart. Local validation is host execution; the Windows gate
and real remote-process/OS process-control behavior were not executed here.

The older activity-finish and component-caller fixtures now declare the synthetic
activities their records represent. The identity fixture's pending process now
captures its declared package owner, matching real process-start admission.
These changes keep the existing regression scenarios valid without adding
production allowances for malformed fixture records.

Local follow-up validation passed: 239 lifecycle checks in each mode; the existing
211 enabled-state checks in each mode plus separate write/read JVMs; 431 service
rebinding checks; 170 activity-finish checks; and 177 ComponentCaller checks plus
16 bootstrap checks. The full application and all service fixtures compiled
against the pinned framework/core classes and passed D8. Both old-head lifecycle
negative controls compiled and failed at their behavioral assertions, rather
than at a missing method or host initialization boundary.

The implementation and all service fixtures compile against the real pinned
framework/core classes, followed by D8. Host tests execute those actual app classes
and the real Android public wrapper, with constructed parsed manifests and explicit
native initialization/identity boundaries. Host cache tests use AOSP's supported
process-local `PropertyInvalidatedCache` test mode and verify cached public
`PackageInfo` values change; production cache code is not bypassed.

- `tools/test_package_enabled_store.py`: 47 checks against the exact production
  store. Covers writer/read bounds, immutable input, serialized-size/UTF failure,
  failed replacement, restart, marker retirement, stale-row compaction, corruption,
  and transient isolation using real files.
- `tools/test_package_enabled_state.py`: 211 checks per protected/GMS mode, plus
  write/read in separate JVMs. Covers public application/component batches,
  validation, metadata/resolvers, actual cache results, concurrent single-package
  and multi-package snapshots, broadcasts, update/uninstall/quarantine rollback,
  failed persistence, install-owner preparation/registry rollback, nonexistent-user
  API ordering and standalone isolation. It also runs the real activity
  manager's exit-dispatch method and `ActiveServices` death/restart decisions with
  a recording application thread and controlled restart timer. Reentrant real
  `registerAttachedProcess` calls exercise stale-record supersession, and recording
  native boundaries verify expected creation identity and Binder fallback outside
  the activity-manager monitor.
- `tools/test_zygote_child_lifetime.py`: 24 checks using the verbatim native
  `ChildStartTime` and `KillChild` functions with recording Win32 process boundaries,
  including missing/zero/mismatched identity, query failure, exited/consumed handles
  and concurrent registry replacement through termination. AddressSanitizer and
  UndefinedBehaviorSanitizer passed locally; LeakSanitizer was disabled because
  this environment's ptrace boundary prevents it from running. CI keeps the normal
  sanitizer invocation.
- The same unchanged public fixture against the baseline app jar fails with
  `Application entry in public batch was ignored` and `Restart lost enabled states`.
  These are behavior failures after compilation, not missing-helper failures.
- Existing regressions: package visibility 58 protected / 22 unprotected checks;
  activity options 114; service rebinding 431; activity finish 170.

Normal CI runs the portable store checks and framework-backed host checks. The
Windows track runs the fixture through `ServiceFixtureRunner`, real ART/JNI,
generated `IPackageManager` proxies/Parcel, and a writable shared cache region,
in both modes and separate persistence VMs. The Windows fixture is compiled here
but has not been executed locally. In-process Binder proxies do not establish
remote-process transport or OS process-kill coverage. Filesystem-failure tests and
separate-VM restart tests do not establish power-loss durability of host hardware.
The complete `services_zygote.cc` Windows translation unit compiled with the pinned
LLVM-MinGW 20261006 toolchain and warnings as errors. Full AOSP native JNI headers
are not installed locally, so `services_natives.cc` was validated by compiling its
exact new callbacks/table rows with the production System V thunk and checking
both descriptors against `javap` of the rebuilt application jar. The complete JNI
translation unit remains part of the normal Windows build.

## Why F07 was not folded into this change

The initial F07 assessment confirmed that UID allocation, dangerous-permission
defaults and several caller checks depend on protected/GMS mode. Removing only
the Java guards is insufficient: the native runtime caches `GESHER_UID` at first
use, while standalone `Main` bootstraps ART before resolving the APK identity.
Always assigning distinct package UIDs therefore requires a coherent launch and
re-execution/provisioning design so `Process.myUid`, Binder identity and installed
metadata agree. That cross-runtime identity migration is intentionally left for a
separate change; this PR gives F25 a complete, independently testable persistence
and publication boundary.
