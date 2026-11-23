# Content observer dispatch: indexed selection with ordered broad fallback

Scope: roadmap #56 F14/F15/T12. This is a bounded runtime improvement; it does not
complete MediaProvider, the built-in provider inventory, or end-to-end performance
acceptance.

## Production change

`ContentService.notifyChange` previously compared every changed URI with every
registered observer. A notification about one row did the same registry-wide
matching work as a complete provider invalidation. The registry now maintains a
scheme/authority/decoded-path index. A narrow change visits its ancestors and
subtree; unrelated siblings and authorities are not scanned. Selection keeps
registration order, repeated registrations, input URI multiplicity, one callback
per Binder, self-notification policy, and batch-validation-before-delivery.

The index is not an authorization cache. The existing owner/registration URI and
changed URI permission checks run both at collection and at delivery. Binder link,
unlink, death-before-publication, stale death, remote failure and reentrant
unregistration still use their existing ownership rules and monitor. Retiring the
last observer prunes its path. Traversal and pruning are iterative, including a
12,000-segment path. Keys preserve null scheme/authority handling in the unmanaged
legacy profile; managed URI validation is unchanged.

Broad invalidations use the original ordered registry when the subtree plus
ancestor registrations could cover at least half of it (minimum 16 entries).
These calls retain the linear matcher and avoid a second full registry copy and
sort. When a subtree contains every live registration and the skip flag is absent,
the index proves the URI predicate for the entire ordered registry; that selection
can omit the redundant matcher while retaining every authorization and delivery
check. Exact leaves can reuse their node's registration list. Empty selections do
not allocate a callback grouping table.

### Android contract repair

Android 16 r4's `ObserverNode.collectMyObserversLocked` applies
`NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS` to descendants-enabled subscriptions at the
changed URI and below it. It still delivers to exact-only subscriptions at those
locations; ancestor subscriptions requesting descendants also remain eligible.
The former matcher always notified equal URIs and suppressed all descendant
registrations under this flag. Both the observer path and the shared JobScheduler
matcher now follow the pinned Android rule. The existing scheduler assertion
which encoded the old behavior now requires the correct callback.

Reference: [Android 16 r4 ContentService](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/content/ContentService.java).
The source SHA-256 is
`0efb8bfa6fc2fd39a15865b3ad27357253f2424e3d882581062ba9485c5510e2`.
The test oracle compiles seven unchanged upstream URI-selection and registration
methods. Entry storage, logging, Binder-death setup and result collection are
declared adapters; none implements selection. This is an AOSP source component
comparison, not an Android device run or a full Android ContentService execution.

## Measured workload and tradeoffs

Baseline: `6afcb6e671fc4f2d08a4a98220f9d9a906a327bf`.
Candidate `ContentService.java` SHA-256:
`93f5b3f51bed7975c17476998b45b28ba020fa0a83935570536c86ec8403862d`.
[Raw samples](content-observer-dispatch-2026-11-23.json) retain source identities,
all samples, setup allocation and callback denominators.

This Linux host execution uses the real production service and framework URI
classes with the repository's existing JNI boundaries. The observer callbacks
count every delivered URI. Both revisions must deliver the same counts. Each
workload uses 4,096 registrations, two JVM processes per revision, and seven
measured samples per process in before/after/after/before order. Temurin
21.0.13.1+2 runs with `-Xbatch -Xmx256m`. Warmup is 20,000 notifications for
selective/missing URIs and 2,000 full notifications for broad workloads. Initial
shorter warmups exposed ongoing HotSpot compilation and were discarded. The broad
samples were lengthened from 30 to 1,000 notifications to reduce short-sample noise.
All timed workloads use flag 0 and have identical delivery counts in both revisions;
the skip-flag repair does not change work in these measurements.

| Complete notification workload | Notifications per sample | Callbacks per notification | Median CPU before | Median CPU after | Allocated bytes before → after |
| --- | ---: | ---: | ---: | ---: | ---: |
| One row under 4,096 sibling registrations | 2,000 | 1 | 314.698 ms | 0.435 ms | 1,568,000 → 1,584,000 |
| Authority with no registrations | 4,000 | 0 | 399.793 ms | 1.021 ms | 1,184,000 → 864,000 |
| Parent invalidation reaching every child | 1,000 | 4,096 | 720.713 ms | 544.991 ms | 836,816,000 → 836,844,000 |
| Child change reaching 4,096 ancestor subscriptions | 1,000 | 4,096 | 682.091 ms | 576.178 ms | 836,816,000 → 836,840,000 |

The narrow workloads remove 99.86% and 99.74% of measured CPU time. The parent
invalidation workload removes 24.38% (0.721 → 0.545 ms per notification), while
shared-ancestor dispatch removes 15.53%. Callback output still has O(N) cost for a
notification reaching all N registrations. Allocation increases by 8 bytes per
selective notification and 24–28 bytes per broad notification; missing-authority
notifications allocate 80 fewer bytes each in these samples.

An earlier candidate retained the matcher for every broad invalidation and showed
a 15.16% parent regression. It was not accepted. The complete-subtree selection
above removes redundant URI matching and the longer exact-source measurements
above replace those earlier results. These host component measurements show no
CPU regression in the measured scenarios; they do not establish #178's controlled
Windows release budgets or every possible notification workload.

The index also increases cold registration/setup allocation: about 4.00 → 8.69 MB
for distinct row registrations and 3.57 → 6.75 MB for shared-ancestor registrations.
These counts include URI decoding, class initialization and temporary setup work;
they are **not retained-heap or process-RAM measurements**. Additional index state
is bounded by currently registered paths and is removed on retirement. The tests
verify registry/index agreement and complete pruning after death and unregister.
No whole-app CPU, memory, frame-time or Windows speedup is inferred from this host
component measurement.

## Validation

- 2,760 production observer checks cover exact/ancestor/descendant semantics,
  self notifications, repeated URI batches, decoded segment boundaries, alternate
  schemes, null authority, whole-registry/linear/narrow crossover with repeated
  Binders and flag/self combinations, index ownership, concurrent
  registration/death/unregistration, 12,000-segment paths and existing callback
  failure/lifetime contracts.
- 4,608 comparisons against the pinned unchanged Android selection methods pass;
  the unchanged baseline fails 198 comparisons with the same fixture.
- The complete JobScheduler content-trigger fixture passes 77 checks, including
  the repaired skip-flag contract and actual ContentService wakeup path.
- Full application Java/D8 and full service-fixture Java/D8 builds pass against
  the verified framework/core artifacts; boot linkage reports zero unresolved
  references.
- Existing managed permission/revocation fixtures remain intact. Actual Windows,
  ART and remote Binder execution remain required in full CI. Local fixtures do
  not establish those results or actual device permission enforcement.

The existing `ContentObserverLifecycleTest` invokes the new dispatch fixture, so
both its framework-host entry point and the existing Windows service track run
it. The host entry point also runs the source oracle when the pinned AOSP source
is available; absence is explicitly reported and is not counted as a reference
pass.

## Reproduction

Use the normal built framework/core/app inputs and a JDK:

```sh
python3 tools/test_pending_intent_callbacks.py \
  --java-home "$JAVA_HOME" --framework-classes out/framework/classes \
  --core-classes out/coremod --app-classes out/app/gesher-app-classes.jar \
  --apksig-jar "$APKSIG_JAR" --case content \
  --content-reference-source third_party/aosp/frameworks/base/services/core/java/com/android/server/content/ContentService.java

python3 tools/test_pending_intent_callbacks.py \
  --java-home "$JAVA_HOME" --framework-classes out/framework/classes \
  --core-classes out/coremod --app-classes out/app/gesher-app-classes.jar \
  --apksig-jar "$APKSIG_JAR" --case content-work \
  --content-work-kind selective --content-work-observers 4096 --content-work-iterations 2000
```

Use `miss`/4,000 iterations, `broad`/1,000 or `ancestors`/1,000 for the other workloads.
Add `--source-ref 6afcb6e671fc4f2d08a4a98220f9d9a906a327bf` for baseline production.
Use `--case content-reference --source-ref ...` for the independent Android
negative control; its 198 mismatches must produce a nonzero exit.

## Inherited workflow reference repair

The full fork workflow initially could not resolve the detached framework recipe
pin `06d175a84affff8914c00151269b0aa2f7f4032a`. Its framework workflow is byte-identical
to the reachable upstream commit `6afcb6e671fc4f2d08a4a98220f9d9a906a327bf` (both have
Git blob `834692396e307a6cd9251c527b7a91ab402d13ac`). The one-line pin correction
retains that exact recipe, all gates and all source/provenance checks.
