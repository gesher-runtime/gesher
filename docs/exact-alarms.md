# Exact alarms and Alarms & reminders Settings

Gesher's exact-alarm access follows the pinned Android 16 `android-16.0.0_r4`
contracts. A manifest declaration, an AppOps decision, and a runtime permission
grant are different kinds of state. `SCHEDULE_EXACT_ALARM` is a signature,
privileged, app-op permission; it is not a dangerous permission that an app can
obtain with `requestPermissions`.

## Scheduling and permission state

| Caller | Exact PendingIntent alarm authority |
| --- | --- |
| Target SDK below 31 | Android compatibility allowance |
| Target SDK 31–32, declaring `SCHEDULE_EXACT_ALARM` | Default or allowed AppOps mode |
| Target SDK 33 or later | Granted `SCHEDULE_EXACT_ALARM` special access, or `USE_EXACT_ALARM` |
| Core UID | Android's core-UID exemption |
| Direct `OnAlarmListener` | No exact-alarm permission required; one-shot listener lifetime is required |

The shared `ExactAlarmAccess` policy drives scheduling, queries, and Settings.
The AppOps service owns the durable mode and verifies the installed generation
when Settings commits a change. Shared UID permissions and package ownership use
the normal package and permission services. Gesher does not treat the absence of
Windows Doze restrictions as a power-allowlist grant.

Pending listeners are death-linked from registration through dispatch. Repeating
listeners are rejected. Effective special-access revocation cancels exact alarms
and retires the affected UID's authenticated process generations while preserving
normal service/activity recovery and inexact alarms. The revocation path preserves
valid requests waiting for a process that had not attached yet. A later grant sends
the protected permission-state broadcast; revocation does not send a grant event.

Positive repeating intervals use Android 16's default bounds: at least 60,000 ms
and at most 31,536,000,000 ms (365 days). Zero and negative intervals retain
one-shot behavior. Registration normalizes the stored interval before timer
dispatch; these bounds do not claim Windows suspend/wake support.

## Native Settings integration

When no installed `com.android.settings` package owns the namespace and the native
backend is available, PackageManager exposes the implemented Android Settings list
and app-detail components. The real intent filters, priority ordering, selectors,
direct-boot classification, and activity-options results agree with dispatch.
An installed Settings package takes precedence over the native components.
The built-in screen is available in the device's system-server process, where
the runtime can reserve and restore the originating task and show the native
window. In standalone mode the native role rejects this operation, so the
built-in Settings component stays unresolved.

The Windows screen shows the actual selected package's access and applies an
explicit user decision through the server AppOps service. Dismissal preserves
state. A decision captured before package replacement cannot mutate its replacement
or return an accepted grant. A committed app-specific grant returns `RESULT_OK`;
dismissal, denial, or an invalidated choice returns `RESULT_CANCELED`.

The launch origin and the activity receiving a result are separate from the task's
top activity. Opening Settings pauses/stops the activity it covers. Closing it
restores the current task top without bringing a stopped result recipient to the
front. Such a recipient keeps its result pending until its normal resume. A
`NEW_TASK` launch cancels the result dependency immediately, as ActivityStarter
does. `NO_USER_ACTION` suppresses the user-leaving hint while retaining lifecycle
transitions.

`FORWARD_RESULT` transfers the source activity's original recipient, result-who
and request code, including a finishing trampoline's queued result ownership.
Only that source's pending result is consumed; another child's result remains
even if its who and request code match. Combining forwarding with a nonnegative
new request code returns `START_FORWARD_AND_REQUEST_CONFLICT` before reserving a
window or changing lifecycle state. A forwarded `NEW_TASK` launch immediately
cancels the original route once. A valid forwarded `NEW_DOCUMENT` request retains
its result dependency, as Android's launch-flag normalization requires. A stopped
recipient or one retained as saved history after process death receives its result
through ordinary delivery/restoration; a retired activity receives no result.

Each accepted native Settings request owns a bounded server reservation from
admission through its coordinator's completion. This includes requests queued
behind another Settings window and `NEW_TASK` requests without a source activity.
Admission clears any earlier device-idle interval; only the last activity or
native UI owner ending starts a new interval. Rejected submissions, backend
failures and worker errors release their own reservation and admission slot once.
No synthetic ActivityRecord is created for this accounting.

The request represents the system Settings component, not an activity running in
the caller's process. Caller death therefore does not dismiss an already accepted
Settings request or make its open native window idle. Ordinary activity-history
and result rules still decide whether the caller can later be restored or receive
a result. This follows the Settings manifest's separate application/component
ownership and `WindowProcessController.handleAppDied`, which processes activities
belonging to the process that died. It does not transfer the caller's authority
to another package; a Settings edit still checks the selected installed generation.

Settings provides opaque coverage of its selected task, including activities
below a translucent task top. The visibility query, deferred-start collection,
final make-visible transition, launch/resume gates and late pause acknowledgment
all observe that coverage. A different task resuming cannot restart an activity
still covered by Settings. Closing the last covering request restores the current
task top and starts any lower activities that are visible again. Other desktop
tasks retain normal multi-resume behavior.
Coverage and restoration retain the actual registered task generation. Removing
that task and reusing its numeric ID cannot let an old Settings window cover or
resume the replacement. Restoration rechecks that generation after result
delivery, which can itself remove the task.
Native launches without a result token still capture the focused task beneath an
existing native window. Selecting only a currently resumable activity would lose
that coverage when another Settings screen or chooser already covers the top.

The integrated intent and document choosers keep their existing translucent
coverage and caller-validity rules. If Settings closes while a chooser remains,
the underlying stopped activity becomes visible without resuming; the chooser's
later completion resumes it. Either completion order retains the other native
operation's busy lifetime and result route.
The existing account-chooser map also participates in the shared idle predicate.
Its original source-validity checks remain in force through caller retirement;
the native call must return before its reservation is released. Its coordinator
now releases through `finally` on worker errors as well. The account idle omission
and missing error cleanup were inherited integration issues, separate from the
two exact-alarm Settings defects; account permissions and selection policy are
unchanged.

## Validation boundaries

`tools/test_exact_alarm_contracts.py` runs the production Java policy, AppOps,
Settings coordinator, resolver, and activity lifecycle against actual framework
classes. Only parsed package/process identities, host-window choices, native
allocation/identity facilities, host locale/service location, and timer dispatch
are isolated fixtures. The
scheduler suite separately checks pending/delivery listener lifetime regressions.
Additional bounded controls exercise queued and headless requests, exhausted
admission, failures, caller retirement, opaque coverage below a translucent top,
stale visibility work, late pause acknowledgment, overlapping Settings requests,
forward-result transfer and conflict, result delivery after process death, task-ID
reuse, and Settings with intent/document/account choosers. The account controls use empty isolated
metadata and a controlled host-call boundary; they make no account-data, credential,
authenticator or real Windows-window execution claim.

Windows track 94 runs the public alarm/AppOps contracts through generated Binder
transport. The existing native `permission_dialog_test` also exercises the actual
Settings controls in English and Hebrew. Linux host success does not establish
Windows native compilation, dialog execution, OS process termination, or timer
suspend/wake behavior; the latest commit's Windows CI remains a required gate.
The grant broadcast's flags are implemented; this change does not claim to add the
runtime's general background-FGS enforcement or its temporary allowlist policy.

Bounded registration-only tests verify repeating interval normalization around
both Android defaults, plus zero, negative and long-integer extremes. They do not
run a rapid repeating timer or claim delivery-load or suspend/wake validation.

## Android source evidence

- [AlarmManagerService](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/alarm/AlarmManagerService.java): access checks, direct listeners, grant broadcasts, and revocation.
- [AlarmManager](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/framework/java/android/app/AlarmManager.java): target-SDK compatibility changes and public API contracts.
- [Framework permissions](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/res/AndroidManifest.xml): permission protection levels.
- [Settings manifest](https://android.googlesource.com/platform/packages/apps/Settings/+/android-16.0.0_r4/AndroidManifest.xml) and [AlarmsAndRemindersDetails](https://android.googlesource.com/platform/packages/apps/Settings/+/android-16.0.0_r4/src/com/android/settings/applications/appinfo/AlarmsAndRemindersDetails.java): components, filters, UID mode changes, and activity results.
- [ActivityStarter](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityStarter.java), [TaskFragment](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/TaskFragment.java), and [ActivityRecord](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityRecord.java): task selection, pausing the resumed activity, and deferred result delivery.
- [WindowProcessController](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/WindowProcessController.java): process death handles that process's activity list; a separate Settings component does not belong to its caller's process.
- [EnsureActivitiesVisibleHelper](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/EnsureActivitiesVisibleHelper.java): compute complete occlusion before making an activity visible or restarting it; invisible activities take the invisible branch.
- [Settings themes](https://android.googlesource.com/platform/packages/apps/Settings/+/android-16.0.0_r4/res/values/themes.xml): the Settings components inherit the ordinary Settings application theme rather than a translucent chooser/dialog theme.
- [ChooseTypeAndAccountActivity](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/accounts/ChooseTypeAndAccountActivity.java): authenticate the launching identity and finish with a cancellation or explicit account selection. Native busy accounting does not grant account access.
