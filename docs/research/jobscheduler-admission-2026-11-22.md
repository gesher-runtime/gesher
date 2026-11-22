# JobScheduler admission accounting and overload bounds

This implements the scheduled-job admission leaf of #56 F30 and records a T12
resource/work bound. It does not complete either roadmap row. The measured
improvement concerns an application exceeding Android's admission limit; it is
not a claim about faster ordinary jobs, Windows CPU or application startup.

## Android contract and existing gap

The baseline is `324dc7e41ea13ceedc3d52db3c8f7c2f3590e452`. Its
`JobSchedulerService.MAX_RUNNING = 4` limits simultaneous execution, but every
valid `schedule()` still appends a distinct pending job. Those records remain in
memory and participate in later scheduler passes and pending queries.

Android's public [JobScheduler documentation][api] describes a 150-job limit
since Android S. The actual pinned Android16 r4 [service][service] checks
`countJobsForUid(callingUid) > 150` **before** inserting a new job. Consequently,
it accepts 151 records and rejects subsequent ordinary schedules, including
replacements. Its identical-`JobInfo` enqueue fast path runs before this check.
This change deliberately preserves that implementation boundary, its exception
type and its message; it does not silently reinterpret the public documentation
as a different observable cutoff.

The pinned [JobStore][store] counts work by scheduling UID and includes only
records whose scheduling and source UIDs agree. Namespaces share that budget.
Explicit privileged `scheduleAsPackage()` bypasses the admission check, but a
same-UID attributed job still contributes to the scheduling UID's count.
Cross-UID attributed work does not. Ordinary system-UID schedules are not
blanket-exempt. Gesher's existing caller authentication, service ownership,
`BIND_JOB_SERVICE`, persistence, network and user-initiated permission checks
remain authoritative.

## Implementation and ownership

Each job records the UID to charge, or `-1` for cross-UID attributed work. One
UID-to-count map is maintained with the actual scheduled-job registry under the
scheduler monitor. Publication, replacement, cancellation, package removal and
terminal completion use the same add/remove helpers. A duplicate or obsolete
run callback cannot decrement a replacement because removal must first find the
exact old record in the registry. Retrying and periodic jobs retain their
reservation. Zero counts remove their map entry.

The quota check follows the existing identical-info enqueue path and precedes
new job allocation/publication. A rejected replacement leaves the old record,
run, work queue and next work ID intact. Shared-UID packages and namespace changes
cannot create separate admission budgets. Normal admission adds constant-time
accounting; this is additional bookkeeping for requests already within Android's
limit, not a universal latency optimization.

GJB3 persisted records carry the admission UID in addition to their existing
installed-owner proof. Valid GJB2 records still load and are conservatively
charged to their verified owner UID because that older format did not record
scheduling attribution. Restore does **not** discard valid records to reach a
quota: it publishes all of them, then uses those counts for subsequent ordinary
admission. Truncated or invalid negative GJB3 admission metadata is rejected.
This does not add persisted JobWorkItem support or change Android's intentionally
unbounded nonpersisted work-item queue.

## Production-path comparison

The same fixture submits 10,000 distinct nonpersisted latency-constrained jobs
for one authenticated application. It then runs 32 actual scheduler passes and
32 actual pending queries. Package/process metadata and native host boundaries
are constructed explicitly. A counting `ArrayList` iterator wraps the real
`mJobs` registry; no scheduler response or admission result is replaced.

| Observed work or retained state | Baseline | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| Accepted and retained jobs | 10,000 | 151 | 98.49% |
| Rejected submissions | 0 | 9,849 | Android admission behavior restored |
| Registry visits in 32 scheduler passes | 640,000 | 9,664 | 98.49% |
| Registry visits in 32 pending queries | 320,000 | 4,832 | 98.49% |

The pass visits include both production registry traversals; pending queries
traverse it once. These counts establish a retained-record and repeated-work
bound for this overload case. They do not measure bytes of RSS, exception cost,
whole-call throughput, native observations or physical hardware power.
The baseline negative-control run of the contract fixture fails with
`Over-limit job was admitted`. The [machine-readable record][data] includes
source hashes, dependency limits and the composition identity.

## Validation and required gates

The candidate passes the executed default `test_scheduler_work_contracts.py`
entrypoint: 238 admission checks, 401 checks in the shared constraint harness
(including the four start assertions reused by the admission fixture), and 155
scheduler-work checks. Coverage includes the exact cutoff, namespaced equal IDs,
shared UID budgets, expedited-job counting, failed replacement atomicity,
enqueue behavior, system/attributed ownership, real completion callbacks,
periodic/retry retention, package retirement, eight concurrent submitters and
eight concurrent cancellation/admission workers.

The unchanged abandoned-job assertion initially exposed an incomplete local
dependency setup: with no AOSP source checkout, `CompatChangesGen` emitted only
three app-local changes and omitted `JobParameters`' source-retained change ID.
The pinned `android-16.0.0_r4` JobScheduler framework archive (23 Java files) was
fetched and supplied through `AOSP_DIR`. The resulting table has 16 changes and
the complete existing scheduler suite passes. No assertion was relaxed and no
production compatibility flag was forced. This remains a source subset, not a
complete platform build/catalog.

The full app and services tests compile to Java/DEX against the actual Android
framework classes and libcore. The existing connectivity suite also passes
(80 callback and nine snapshot checks). The immutable framework recipe
`8b316aebf09ab46d5407e955bf86ed748a1e739f` already invokes the candidate scheduler
helper with its default `all` case, so the new host gate executes without
changing that trusted recipe pin.

The existing Windows/ART `JobSchedulerConstraintContractTest.main()` now invokes
the admission fixture before its terminal success marker. Its real-Parcel branch
round-trips GJB3 scheduling attribution, restores 152 valid legacy over-limit
jobs through the actual scheduler `start()` path, checks post-restore admission,
and rejects truncated/malformed GJB3 records. Host output explicitly says
`persisted_parcel=false`: its native adapter accounts Parcel write sizes and
does not provide real Parcel serialization. Those target runtime assertions are
compiled and required but have **not** been executed in this Linux environment.

## Composition with open PR271

The independently published connectivity observation PR was checked at
`a083bc97a66b7544ed2fa0719ba6241215d8f6cc`. Its source was composed with current
main and this candidate in an isolated worktree; it is not included in this PR.
The tested local composition is `0f55c625e901a7a0bb19e4f87a6195ea98ea2bee`,
tree `e517ad409bb9493f401d804e358339e8ce6d37f2`.

Three cancellation/removal merge sites must retain **both** admission-count
retirement and `updateNetworkObservationLocked()`. The target main must invoke
both connectivity and admission contracts. The connectivity helper now supplies
the fixture source directory to javac so the added fixture dependency resolves
when PR271's helper compiles the shared scheduler test.

The resolved composition passes the full app/services Java/DEX builds, the same
238/401/155 scheduler gates, 80 callback and nine snapshot checks, and all 74
connectivity-demand checks with a maximum of one controlled native waiter.
Its host persistence boundary remains explicitly false. This is local source
composition evidence, not a claim that remote PR CI or Windows execution passed.

## Reproduction

Use the pinned framework/core dependencies, a JDK17+ and apksig8.13.0. Build the
app with the pinned JobScheduler framework sources available to
`CompatChangesGen`, then run:

```sh
python3 tools/test_scheduler_work_contracts.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" \
  --apksig-jar "$APKSIG_JAR"

python3 tools/test_scheduler_work_contracts.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" \
  --apksig-jar "$APKSIG_JAR" --case admission-work \
  --source-ref 324dc7e41ea13ceedc3d52db3c8f7c2f3590e452 --expected-jobs 10000
```

The same baseline with `--case admission` is an expected failing negative
control. The source-ref path compiles the older production service and related
scheduler classes, while keeping the fixture and host boundaries identical.

[api]: https://developer.android.com/reference/android/app/job/JobScheduler
[service]: https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobSchedulerService.java
[store]: https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/apex/jobscheduler/service/java/com/android/server/job/JobStore.java
[data]: jobscheduler-admission-2026-11-22.json
