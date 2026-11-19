# AppOps modes and data delivery

Gesher's AppOps Binder and server consumers use the same `PackageManagerService.appOps()`
instance. `Services.create` registers the permission checker in both server and
in-process modes. Package permission decisions and AppOps decisions are separate:
a granted runtime permission can still produce `PERMISSION_SOFT_DENIED` when its
operation is ignored. A permission with the Android `appop` protection flag uses
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

## Policy and ownership

An unset operation has `opToDefaultMode(op)`. Explicit `MODE_DEFAULT` is retained
when it differs from that operation's initial mode. A nondefault UID mode takes
precedence over the package mode. Aliases use `opToSwitch`, while reported
operation events retain the requested operation. A normal initial-allowed start
works with `startIfModeDefault=false`; accepting an explicit default is a separate
caller choice.

Mode writers require `MANAGE_APP_OPS_MODES`. Note/start/finish requests authenticate
the incoming UID and the installed package; delegated attribution follows the
platform's trusted-source rules. Statistics require their framework permission.
The runtime has no virtual-device implementation, so unknown devices receive
`MODE_IGNORED` rather than being silently treated as the default device.

Persisted modes include the full UID, package name and verified signer identity.
A snapshot is published after its atomic file write. Reads do not wait on the
operation monitor for disk I/O. Legacy experimental files without signing
ownership are not used to confer grants. Uninstall retires ownership, and a
signing change cannot inherit old modes. Invalid input is not partially loaded.
The server-local generation-checked UID setter makes a desktop special-access
decision conditional on the same package still being installed.

## Lifetimes and callbacks

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
mode persistence.

The Windows `framework-fixes` track runs the fixture through generated AppOps and
PermissionChecker Binder Parcel transports. Its required success marker includes
`binder=true`. `tools/test_appops.py` runs the same service contracts on a host JDK
with the existing declared JNI identity/clock boundary. Its host-only setup also
supplies the pinned CAMERA/VIBRATE manifest definitions when framework assets are
unavailable, bypasses unrelated ICU initialization and disables the cross-process
cache nonce. An isolated broadcast transport boundary keeps package lifecycle tests local.
It does not replace permission decisions or operation methods.

A host result does not establish ART, remote-process Binder death, Windows window
visibility, foreground-service process execution or hardware delivery. The
current commit's Windows CI remains required before automerge.
