# Package visibility: reuse admitted metadata and index app-ID groups

Scope: #56 F49/T12, limited to the cost of the visibility paths that already use
`PackageVisibility`. The wider caller/target filtering coverage and interaction
graph in F49 remain open.

## Problem and production change

A public `canPackageQuery`, search visibility lookup or filtered install-source
lookup rebuilt the same installed-package map and visibility metadata on every
call. The authenticated installed-service lookup did the same work separately.
Each `canSee` could then scan the inventory three more times to find the caller's
UID peers, a force-queryable target peer, and the target's manifest-query peers.

`PackageManagerService` now retains one snapshot of admitted packages and pending
removals. Its public and installed-only definition sets are constructed separately
and lazily, preserving their different synthetic-host membership. The metadata is
reused while both authoritative map identities remain the same. A package
publication, removal start, removal end or recovery clears the snapshot
immediately, even if no query follows.

Public snapshot construction retains its original position outside the method's
PMS monitor. This matters because the existing synthetic GMS metadata factory can
probe its code path. Construction captures a coherent generation under the
monitor, then publishes its result only if that generation is still current. A
concurrent publication may retire an in-flight query's captured metadata; that
query retains the original coherent-snapshot behavior and cannot install its old
snapshot back into the cache. Callers that already held the monitor retain their
previous locking behavior.

`PackageVisibility` indexes app IDs to their entry groups. Entries link their UID
peers without a separate list/backing array for each isolated application UID.
Groups are reversed once during construction to retain the former installed
iteration order. Hash lookup avoids both repeated full-inventory walks and the
insertion cost of an ordered sparse array when UID order differs from package
order. Construction is bounded by current metadata, and ordinary lookups inspect
only actual source/target peers. Large shared-UID groups still require their
actual declaration work.

This caches metadata, **not visibility decisions**. The following stay live:

- `ApkPackage.installSource()` and the installer/initiator's current installation
  identity, including reassignment without package publication.
- System-addon status and the current force-queryable framework resource policy.
- The platform protected-broadcast predicate.
- Caller identity, user checks, typed missing-package failures and each query's
  fresh positional result array.

Admitted UIDs and manifest metadata are read when a snapshot is built. The normal
UID admission path assigns them before package publication; a replacement uses a
new `ApkPackage`. The publication and all three pending-removal transitions were
audited explicitly. Enabled-component state does not affect these existing
manifest visibility edges and is not cached as an authorization result.

## Android contract and nearby work

The predicate's existing Android rules are unchanged: core/self/shared UID,
legacy target, `QUERY_ALL_PACKAGES`, declared packages/intents/providers,
permissions/libraries, trusted force-queryable packages and installer attribution.
The same actual `PackageVisibility` methods perform those checks. Relevant pinned
Android references and the original public-operation contract are documented in
[the visibility contract report](package-visibility-followup-2026-11-08.md),
including Android 16 r4's
[AppsFilterBase](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/AppsFilterBase.java),
[AppsFilterImpl](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/AppsFilterImpl.java)
and
[ComputerEngine](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java).
This change does not manufacture an interaction graph or broaden unrelated
package enumeration APIs.

Open PRs were inspected before implementation. #122 (UserDictionary, audited
head `693d5bf1a7c859a7ec59d2b4579f38f86e24d4ee`) and #295 (Contacts, audited head
`e2be211be595b8915e67c9d46290dda4feaee90c`, stacked on #191) add provider/host
metadata and policy, not this cache or app-ID index. Their host additions must
remain before group-index construction when composing the changes. This report
is not a qualification of those independent providers or their open CI work.

## Measured production work

Baseline: `df3aff782e2d411aad2d07ca39f0b21ee2ddbfe2`.
Candidate SHA-256 values:

- `PackageManagerService.java`:
  `1d0b820622116517906c15d7e7836117d2569228a67074aa48822b3e95f53f76`
- `PackageVisibility.java`:
  `9072c7cc60b7c77acbaf170aa2e2c3800381391292bca9f57fd47d32ed1fcbba`

[The raw report](package-visibility-snapshots-2026-11-23.json) contains every sample,
both production source hashes for every run, result denominators and the host
runtime identity. These measurements execute the complete framework
`ApplicationPackageManager.canPackageQuery` wrapper, PMS operation and visibility
predicate. Fixtures construct admitted package metadata and use the existing
native identity boundaries. No predicate, query result or collection getter is
replaced in the timed runs.

Each scenario uses two fresh JVMs per source, seven measured samples per JVM, in
before/after/after/before order. Temurin 21.0.13.1+2 runs with
`-Xbatch -Xmx256m`, after `max(100, 2 × iterations)` complete-query warmups. The
benchmark validates result lengths and exact visible-result counts on every
sample. CPU is current-thread CPU; allocated bytes count allocated work rather
than retained heap. Wall-clock samples are retained alongside them.

| Complete public workload | Packages | Calls per sample | Visible results per call | Median CPU before → after | Allocated bytes before → after |
| --- | ---: | ---: | ---: | ---: | ---: |
| One declared target | 4,096 | 1,000 | 1 | 465.854 → 0.149 ms | 559,399,296 → 56,000 |
| One hidden target | 4,096 | 1,000 | 0 | 420.166 → 0.259 ms | 559,408,224 → 78,056 |
| Bulk query-all targets | 4,096 | 1,000 | 4,095 | 578.078 → 105.180 ms | 563,777,336 → 69,632,000 |
| Two large shared-UID groups, no declared edge | 256 | 1,000 | 0 | 160.211 → 120.014 ms | 39,055,296 → 56,000 |
| Small inventory, one declared target | 2 | 20,000 | 1 | 11.188 → 2.012 ms | 49,760,000 → 1,120,000 |
| Package publication followed by its first query | 4,096 | 1,000 | 1 | 439.918 → 307.839 ms | 559,436,248 → 559,376,000 |

The large selective and hidden cases remove 99.968% and 99.938% of measured CPU
work, respectively. Bulk lookup removes 81.81%, large shared groups 25.09%, the
small inventory 82.02%, and the measured cold publication/query cycle 30.02%.
The cold scenario re-publishes constructed admitted metadata and includes the
real publication/invalidation method and first public query; it does not perform
APK parsing, signature admission or Installer disk work. It has no pending
removals. Pending-removal correctness is covered separately below.

Initial sparse-array candidates were rejected after cold measurements showed a
regression. The linked app-ID groups and pre-sized metadata map remove that
regression in the final measured scenarios. No all-workload, Windows/ART or
whole-application speedup is inferred. The controlled Windows performance and
resource gates in #178 remain separate release requirements.

The retained-state tradeoff is explicit: at most one public and one installed-only
metadata view are retained for the current package/removal generation, each with
an app-ID index bounded by that generation's entries. There is no per-query
caller/target cache and no historical-generation cache. Mutation clears both
views immediately. In-flight queries may still own their captured metadata until
they return, as they did with the previous copied snapshot. Process RAM and
retained-heap size were not measured by these allocation counters.

## Validation and limits

- The actual PMS/APM fixture passes 553 checks in each signature-spoofing mode.
  Existing Android rule, caller/user, ordering, duplicate-target, exception,
  static library, permission, provider and installer tests remain in place.
- New lifecycle cases use real publication and removal methods: shared-UID
  removal, rollback, recovery, code replacement, changed UID membership,
  protected-broadcast replacement and immediate cache retirement.
- Same-snapshot tests change installer attribution and a declared resource
  catalog boundary; the actual production predicates must immediately grant and
  revoke visibility without rebuilding metadata. These are explicit test inputs,
  not production resource substitutions.
- Concurrent queries cross 400 publications. A separate deterministic fixture
  blocks an actual metadata getter inside construction while another thread
  publishes a replacement. Publication must finish before the reader is released,
  the old query must remain coherent, and its retired construction must not become
  the next query's cache.
- A production work gate observes metadata getters without changing their return
  values. At 2, 50 and 500 packages, cold queries read 4, 100 and 1,000 metadata
  fields; 64 subsequent queries read **zero** such fields. This gate is separate
  from the uninstrumented CPU/allocation measurements. The baseline fails its
  repeated-query budget.
- The existing production intent-index fixture still passes 763 checks and its
  work budgets.
- Full application and full service-fixture Java/D8 builds pass, with zero
  unresolved boot links across 292 framework references. Full fork CI still must
  supply actual Windows/ART integration. Local host evidence is not an Android
  device, remote Binder, APK parser or Installer run.

The existing framework-host visibility gate compiles the current PMS and
`PackageVisibility` sources together. The existing Windows visibility fixture
runs the new lifecycle, resource and concurrency checks through its actual Binder
adapter; the metadata-counter work gate runs on the host. No workflow gate was
removed or weakened.

## Reproduction

Use the normal built framework, libcore and application classes:

```sh
python3 tools/test_package_visibility.py \
  --java-home "$JAVA_HOME" --framework-classes out/framework/classes \
  --core-classes out/coremod --app-classes out/app/gesher-app-classes.jar \
  --apksig "$APKSIG_JAR"

python3 tools/test_package_visibility.py \
  --java-home "$JAVA_HOME" --framework-classes out/framework/classes \
  --core-classes out/coremod --app-classes out/app/gesher-app-classes.jar \
  --apksig "$APKSIG_JAR" --work selective \
  --work-packages 4096 --work-iterations 1000
```

Other measured inputs: `hidden`/4,096/1,000, `bulk`/4,096/1,000,
`groups`/256/1,000, `selective`/2/20,000 and `cold`/4,096/1,000. Add
`--source-ref df3aff782e2d411aad2d07ca39f0b21ee2ddbfe2` for the unchanged baseline.
Use `--work-check` to enforce metadata-read budgets; with the same baseline
`--source-ref`, it must fail because repeated queries still rebuild metadata.

The inherited reusable framework-workflow pin is repaired from detached commit
`06d175a84affff8914c00151269b0aa2f7f4032a` to reachable upstream
`6afcb6e671fc4f2d08a4a98220f9d9a906a327bf`. Both contain exact workflow blob
`834692396e307a6cd9251c527b7a91ab402d13ac`; the recipe and all mandatory gates are
unchanged.
