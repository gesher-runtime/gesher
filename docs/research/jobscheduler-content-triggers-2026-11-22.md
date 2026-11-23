# Bounded JobScheduler content-trigger batches

This is the F29/T12 content-trigger batching leaf of [#56](https://github.com/gesher-runtime/gesher/issues/56). It does not complete the broader JobScheduler, observer, quota, or performance rows.

## Android contract and implementation

The reference is **Android 16 `android-16.0.0_r4`**, not an invented global limit on delivered changes:

| Pinned source | Relevant behavior |
| --- | --- |
| [ContentObserverController.java](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/ContentObserverController.java) | A fresh notification batch retains at most 50 distinct URIs, always retains matching authorities, and becomes urgent at 40 URIs. Preparing, retrying, and replacing jobs transfers pending ownership and preserves carried unions. |
| [JobStatus.java](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/controllers/JobStatus.java) | Default update/max delays are 10/120 seconds; explicit delays have 500/1,000 ms minima. |
| [JobServiceContext.java](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobServiceContext.java) | Converts the entire retained URI set to the delivered array. It does not replace retry unions exceeding 50 with `null`. |

The source hashes are recorded in the companion JSON. The public `JobParameters` documentation permits a null URI array when detailed changes are unavailable; this implementation follows the concrete pinned controller/service behavior when it does retain the changes.

Previously, every matching URI remained in `Job.changedUris` while another constraint blocked execution. The first 40 changes were not urgent, explicit debounce delays were not clamped, replacement discarded pending changes, and a run with more than 50 retained URIs discarded its URI array.

The service now bounds each fresh accumulation, retains all authorities, and latches content readiness once the deadline or urgent threshold is reached. Later notifications cannot make an already satisfied content constraint false while charging or connectivity still blocks the job. Deadline addition saturates instead of overflowing.

The pending set and each active run retain separate ownership. A replacement receives the pending set, not the URI array already delivered to the replaced run. A retry keeps the union of that run's changes and notifications received while it was running, including a run that originally started only because its deadline elapsed. These carried unions can exceed 50; the fixture delivers and checks 150 distinct URIs across retries. They are not truncated or converted to null.

The listener wakes the scheduler when a newly matching change brings its due time forward. Extending a quiet period leaves the already scheduled wake to resample it; notifications for an already ready job do not repeatedly wake a scheduler that still cannot admit it. The scheduler monitor protects all batch/readiness changes and replacement/run ownership. Existing trigger matching, authenticated caller checks, admission, other constraints, and logging remain in use.

## Measured retained state

The same workload runs against the actual old and new `JobSchedulerService`, compiled by `--source-ref` for the old control. It submits 100,000 distinct matching notifications in input arrays of 100 to one job whose charging constraint remains unsatisfied. This bounds the fixture's input storage while leaving the production service responsible for retained state.

| Observation | Baseline `6759e71c` | Candidate |
| --- | ---: | ---: |
| Matching notifications | 100,000 | 100,000 |
| Retained URI entries | 100,000 | 50 |
| Retained authorities | 1 | 1 |
| Running jobs | 0 | 0 |
| Charging constraint unsatisfied | true | true |

This is **99.95% fewer retained URI entries for a fresh pending batch**. It is a structural retained-state measurement, not measured RSS, CPU, general throughput, or a bound on carried retry unions. The matching loop and production logging still process the notifications. URI sizes and authority counts are not limited by this change.

The baseline also fails the new semantic fixture at `40 URIs did not trigger urgently`. That negative control exercises the old production path before any candidate-only deadline helper is called.

## Verification and ownership cases

The candidate's existing framework helper entry point, with its default `all` group, passes:

- 77 new content-trigger checks, including 39/40/50/51 boundaries, duplicates, nonmatching changes, authority retention after saturation, minima/default deadlines, saturation arithmetic, and latched readiness.
- Actual `Run.started` / `IJobService` delivery to `JobParameters`, full retry unions, deadline-only initial runs, pending-only replacement transfer, late old-run completion, cancellation/reuse, package removal, and concurrent notification/replacement ownership.
- 405 shared scheduler constraint-harness checks and 155 existing scheduler/work checks.
- The 100,000-notification candidate work gate above.

The new fixture also registers through real `ContentService`, calls its real `notifyChange` listener path, and runs the actual scheduler worker. The urgent batch must be observed within three seconds, below the host worker's 30-second fallback poll, and the single-change batch must wait at least the actual 500 ms quiet minimum. Charging remains unsatisfied and no job is admitted. Worker failure, join/retirement, and results written under the production monitor are checked. This exercises listener wake and timed wake behavior; it is not a measured Windows notification or scheduler-wakeup-count claim.

Existing connectivity checks pass (80 callback checks and 9 service snapshot checks). Full app Java/DEX and shared services Java/D8 builds pass against the actual framework/core inputs. An independent reviewer replayed all 77 content checks and reviewed the production changes against the three pinned Android source files.

The host uses real framework classes and production services with constructed installed-package/process metadata and the declared native identity, clock, allocation, and write-size Parcel boundaries. Most run dispatch is controlled by the fixture; the listener/worker case above uses the real worker. Real Binder/Parcel round trips for the delivered arrays are included in the existing Windows constraint entry point, which calls `contracts(true)` before its success marker. Those target tests compile into DEX but were not executed on Windows/ART for this local result. A complete remote CI result is separate from these local passes.

## Existing CI entry points

No workflow recipe pin is changed. The pinned framework workflow already executes the candidate checkout's `tools/test_scheduler_work_contracts.py`; its default group now compiles and runs the new content fixture. The existing Windows constraint main also invokes it. The connectivity helper receives the existing services source path so compiling the shared constraint fixture continues to resolve its referenced content fixture.

## Composition check

An isolated local union starts at `6759e71cde265d0c76fef8725d8b1ceecd246279`, merges [#271](https://github.com/gesher-runtime/gesher/pull/271) at `a083bc97a66b7544ed2fa0719ba6241215d8f6cc` and [#284](https://github.com/gesher-runtime/gesher/pull/284) at `6beb87479465a57cefddc24def53ef9f2f4e77cf`, then applies this leaf. The content production patch merges cleanly. The earlier two PRs require preserving both admission accounting and connectivity observation updates at removal sites; shared fixture conflicts keep all three test families.

The union is commit `dd965c46ddd61ecf4a22eaecc9f17e6d2930fc07`, tree `cf8805efe33181e5ec0de962b3ea31d35873a5b5`. It passes full app/services Java+DEX builds, 238 admission checks, 77 content checks, 409 shared constraint checks, 155 scheduler/work checks, both structural work gates, 80 connectivity callback checks, 9 snapshots, and 74 demand/ownership checks with one maximum native waiter. This is a local composition diagnostic; the PR does not incorporate the other open PRs. The current #249 head `36f217e0730e939512abff58e4c54c6ca4fae7cb` does not change this service, ContentService, or the shared constraint fixture.

## Reproduce

Use the built framework, core, app, and apksig inputs used by the existing framework job:

```sh
python tools/test_scheduler_work_contracts.py \
  --java-home "$JAVA_HOME" \
  --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" \
  --app-classes "$APP_CLASSES" \
  --apksig-jar "$APKSIG_JAR"
```

For the baseline retained-state comparison, add `--case content-work --expected-uris 100000 --source-ref 6759e71cde265d0c76fef8725d8b1ceecd246279`. For the expected failing urgency control, use `--case content --source-ref 6759e71cde265d0c76fef8725d8b1ceecd246279`. Candidate `--case content` runs the new contract suite alone. Receipt hashes, exact source hashes, and structural result rows are recorded in [the companion JSON](jobscheduler-content-triggers-2026-11-22.json).
