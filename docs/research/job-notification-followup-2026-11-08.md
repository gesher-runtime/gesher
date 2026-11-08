# Job notification lifecycle follow-up — 2026-11-08

## Missing behavior

`JobSchedulerService.Run.setNotification()` posted a notification but ignored
`jobEndNotificationPolicy`. It did not validate the required icon/channel/policy,
retain notification-ID ownership, remove a notification when its final job
completed with `JOB_END_NOTIFICATION_POLICY_REMOVE`, or detach its UIJ flag when
the final user-initiated job ended. A channel guard based only on `mJobs` also
missed a running service once cancellation or replacement removed its scheduled
job entry, before the service acknowledged its stop.

This follow-up implements the normal job-notification association lifecycle using
the pinned Android 16 contract. The reference is
[JobNotificationCoordinator](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobNotificationCoordinator.java),
especially `validateNotification`, `enqueueNotification`, and
`removeNotificationAssociation`.

## Implemented operations and state

The callback now checks its existing job/caller identity and validates a copied
notification: the notification and small icon must exist, its actual channel must
exist, and the end policy must be `DETACH` or `REMOVE`. Invalid input leaves its
previous notification association intact. Completed callbacks retain the service's
existing no-operation completion contract.

Each run with a notification belongs to a separate association set. This set is
independent of the scheduled-job list, so a running service remains associated
while a cancel or reschedule is waiting for its stop acknowledgement. Associations
record the notification ID, actual channel and latest end policy. Channel deletion
checks use the current installed owner and these active UIJ associations.

| Event | Result |
| --- | --- |
| A run changes its notification ID | Its old association is released using the old end policy before publishing the new ID. |
| A run updates the same ID | Its channel and end policy are updated without acquiring a second association. |
| One of several jobs sharing a package/notification ID finishes | The notification remains while another job still uses it. An earlier `REMOVE` policy cannot cancel it. |
| The final associated job finishes with `REMOVE` | The active notification, retained click/action record and host toast are canceled through the notification service. |
| The final associated job finishes with `DETACH` | The notification remains. Its UIJ flag is removed once no UIJ still owns it. |
| The last UIJ finishes while an ordinary job still uses the same ID | The notification remains for the ordinary job, with its UIJ flag removed. |

UIJ flags derive from actual current associations, including when an ordinary job
updates a notification shared with a UIJ. An app-supplied flag cannot create a UIJ
association. Delayed runs from a removed owner are retired without mutating a new
package's notification after its package-removal epoch changes.

Association updates, notification publication and completion use the notification
service's lifecycle fence. Its listener dispatch is deferred until the outer fence
is released. The scheduler still releases its monitor before calling notification
operations, and performs the existing service-unbinding and persisted-job cleanup
in a `finally` path after a completion transition. A notification/listener failure
therefore cannot strand a completed service binding.

The notification service's detach helper clears the live UIJ flag, updates the
retained record and informs listeners. This follows the pinned
[NotificationManagerService flag-removal path](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/notification/NotificationManagerService.java).

## Validation and limits

The changed `JobSchedulerService`, `NotificationManagerService`,
`NotificationStore`, `ActiveServices` and `ActivityManagerService` compile together
against the actual CI framework classes and Android core module using Java 21,
target 17. The new `JobNotificationLifecycleTest` also compiles against these
production replacements. `git diff --check` is clean for the scheduler change.

The new Windows/ART fixture uses a genuinely parsed `devsvc.apk`, production
package/job/notification services, generated `IJobCallback` proxy transactions and
the retained notification store. It covers invalid inputs, wrong job IDs, ordinary
and UIJ flags, both completion policies, repeated completed callbacks, two UIJs
sharing an ID, ordinary/UIJ sharing, changing IDs and channels, and a canceled job
whose scheduled entry is gone while its real callback awaits stop acknowledgement.
It checks both live flags and retained records. The host-toast sink is deliberately
injected; this fixture tests service publication/removal, not Windows toast rendering.

The fixture requires `GESHER_BINDER_UID=1000` so the production retained-record
path is active. It has not been executed locally on Windows/ART, and compilation
is not reported as a runtime pass. The existing package host tests are independent
evidence and do not substitute for this job fixture.

This is bounded normal-completion parity. Android additionally forces removal for
actual user-stop and background-restriction stop reasons. Gesher's existing
`stopRun` path does not carry those distinct reasons, so this change does not claim
to implement them. It also does not add Android's UIJ quota/permission scheduling
policy, notification quota policy, or a new notification rendering backend.
