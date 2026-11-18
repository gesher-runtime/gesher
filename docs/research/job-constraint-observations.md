# Job constraint observations and history

The scheduler maps observed Windows power, network, idle and storage state to
Android job constraints. Missing host state does not satisfy a requested
constraint. This implementation keeps the application's JobService, callback
order, constraint reasons and job identity; it does not substitute a different
operation or fabricate a healthy host.

## Ordering controller observations

Host-event revisions invalidate a sample if an affected input changes while the
sample is read. They do not order two polls when there was no host notification.
Storage pressure is one such input. A poll can find low storage and wait for a
run's dispatch monitor while service admission accepts a newer healthy sample.
The older proposal must not stop that admitted run.

The scheduler therefore assigns a sequence before each controller sample and
records the latest accepted sequence on each job. Both admission and the worker
publish accepted observations. A delayed stop or network proposal must match the
job's accepted sequence as well as its relevant event revision and run identity.
An older unfinished sample cannot replace a later accepted one. Acceptance is per
job, so changes to another job's host inputs do not invalidate its observation.

Native calls and task-manager observation remain outside the scheduler monitor.
Transitions take the run's dispatch monitor before the scheduler monitor; the
worker releases the scheduler monitor before applying proposals or binding.
Application callbacks remain outside the scheduler monitor. A later genuine
constraint loss still stops the run and preserves its requested constraint on
retry.

Network assignment and notification are separate while `onStartJob` is waiting
for acknowledgement. An unchanged later poll keeps the accepted assignment
current so the deferred handover is delivered after acknowledgement. It does not
send a premature or duplicate callback.

## Pending-reason history

Android's `JobStatus.setConstraintSatisfied` records changes before controller
dispatch, including changes while a job is running or awaiting a stop
acknowledgement. Accepted observations are consequently recorded before the
active-run branch. History belongs to one schedule, retains at most ten entries,
uses wall-clock observation timestamps and returns defensive array copies.

History and the current pending-reason query have different ready-state values.
The pinned `JobStatus` history records `PENDING_JOB_REASON_APP` for a ready job
with a resolved service. The scheduler's live query still reports `EXECUTING`
for an active run and `UNDEFINED` for a ready, eligible pending job. Android's
`ContentObserverController.prepareForExecutionLocked` moves delivered URIs into
execution state without clearing the satisfied content constraint. An active
run therefore retains that satisfaction after the pending URI collections have
been consumed; losing charging alone does not invent a new content blocker.

## Source and validation boundary

The source reference is Android 16, `android-16.0.0_r4`, frameworks/base commit
`45034f0663f960d9ee5fb0a101a4732b71f6e2f4`:

- [JobStatus constraint changes, history and readiness](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java)
- [JobSchedulerService pending-reason APIs](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java)
- [ContentObserverController execution transfer](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/apex/jobscheduler/service/java/com/android/server/job/controllers/ContentObserverController.java)
- [Public JobScheduler contract](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/apex/jobscheduler/framework/java/android/app/job/JobScheduler.java)

`JobSchedulerConstraintContractTest` exercises actual `schedulePass` and
`Run.started` paths with a dispatch monitor held across real queued proposals.
The new storage and network races change only the fixture's host input, without
a host event or a manufactured proposal. Further cases cover a genuinely later
loss, deferred network acknowledgement, active and stopping history, its bound
and lifetime, and delivered content-trigger state.

The Linux host runner uses production Java services and framework classes with
constructed package/process metadata, manual admission, controlled native host
inputs and a limited Parcel boundary. A host pass does not qualify Windows/ART,
Binder transport, physical power or storage events, or Android-device execution.
The broader Android job execution-quota and user-visible-observer work remains
outside these observation and history corrections.
