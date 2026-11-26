# Exact package UID queries: reuse admitted metadata and batch cold lookups

Date: 2026-11-23. Scope: the package-identity and permission-query performance leaf supporting [#56 F07/F49/T12](https://github.com/gesher-runtime/gesher/issues/56) and [#178 JAVA-06](https://github.com/gesher-runtime/gesher/issues/178). This depends on [#345](https://github.com/gesher-runtime/gesher/pull/345)'s existing visibility snapshot and group index. It does not complete those roadmap rows.

For a 4,096-package inventory, the measured completed warm name, owner-array, permission and 128-UID batch operations use **99.59–99.95% less caller CPU**. A steady 128-UID batch with no existing metadata view uses **99.12% less caller CPU**; the separately measured first call uses **76.87% less caller CPU**. The final primitive group index removes the prototype's boxing allocations and reduces one actual visibility view's measured structural storage from **557,696 to 393,752 bytes**. These are bounded host-component observations, with the interference qualification below.

**There is no universal performance or release-budget pass.** Tiny warm name queries, core-only batches, small cold batches and cold view construction retain adverse CPU, latency or allocation observations. First-query CPU and wall time sometimes move in opposite directions. The controls above 5% require explicit workload-budget decisions before release; this report does not invent an acceptance threshold to declare them green. Actual Windows/ART/Binder execution, representative applications, whole-process memory and the complete #178 budgets remain required.

The [JSON report and digest inventory](package-uid-query-2026-11-23.json) contains every summary, source/dependency identity and archive-member digest. The [complete evidence archive](data/package-uid-query-2026-11-23.tar.gz) retains all 500 query-process receipts and logs, all four experiments' complete compiled production/fixture source inputs, structural-storage receipts, mutation logs and validation logs. No timing sample is deleted or replaced.

## Source and integration identities

The controlled baseline is published #345 head `d3f72de8a79e2446d41c4bf40824ebbcb42769b4`. Both variants compile **complete** PackageManagerService and PackageVisibility sources with the same actual API fixtures and real framework/core/app dependencies; no matcher or UID resolver is extracted or substituted.

Final production SHA-256:

- `PackageManagerService.java`: `06865ff2991b5ed0a7476ae966e46f9e0d164567ede82d886fe1a91c50f150f5`.
- `PackageVisibility.java`: `4c64fc32f33df3b60cdcba487b1df4f232e7eefcf2d5e0cf2ef63a5ed1779810`.

After measurement, main `fdf4861b608d7a5fe8c0fa6c8a285dfbdf573d91` was composed separately. Its F09 permission-state implementation and producer-authentication gates are retained. Both measured production files remain byte-identical. The canonical merge preserves all 14 branch-only and 139 main-only paths and modes; its only conflict selects main's published `92c87bef...` framework reference. Both the previous `5aa6b390...` reference and that reference contain exactly recipe blob `6dc4cca565aaf15fdf518eee825b0e44765b275f`; the complete resulting CI file equals main. This integrated build/test result is separate from the older controlled baseline's performance result.

The framework compilation dependency is the verified `framework-classes` artifact from successful main [run 45094093107](https://github.com/gesher-runtime/gesher/actions/runs/45094093107), source `d2ebbdfd97699879dd4d746f9e0ce3db23521cf1`, artifact ID `14783141252`, SHA-256 `8e27ecee656f984b7745333bcfb765d0e723dcb38084e05223e5dde7b44da631`. All current application and services sources are rebuilt. That older compile dependency is disclosed; it does not stand in for current Windows CI.

## Why the existing work remains expensive

`getNameForUid` and `getPackagesForUid` scan the admitted package map. `getNamesForUids` repeats that scan for every requested UID. The live public `checkUidPermission` path reaches `getPackagesForUid` before evaluating permission policy. These scans continue even when a visibility query has already built the same generation's package/app-ID metadata.

Building a second permanent UID map would add another retained owner index and another invalidation contract. The change instead borrows an already-valid view owned by the captured visibility snapshot. Identity queries never force a visibility view to be built. If no eligible view exists, individual or small queries retain the direct scan, and only sufficiently large cold batches traverse the registry once.

## Changes and preserved contracts

1. Safely publish the existing snapshot and its independently constructed public/installed-only views through volatile fields. Writers retain their existing monitor. Formerly lock-free UID queries acquire no new PackageManagerService monitor and never borrow a view from a different generation.
2. Reuse the existing app-ID group chain, filtering **full UID equality** and real APK ownership. The app-ID is only a candidate index; another Android user or a synthetic host definition does not confer ownership. Return fresh owner/result arrays in original admitted-package order.
3. Replace the single boxed group map with a private primitive open-addressed table. Zero and negative keys use ordinary occupied slots; empty heads, rather than a key sentinel, identify unused slots. Growth, compaction and reversal happen before publication. The table is read-only afterwards and preserves the existing linked group order.
4. With no reusable view, batches of at least 16 requested UIDs and at least 32 packages collect only query-local requested keys, sort/deduplicate them, and make one original-order registry scan. A repeated single non-core UID needs one scan and no key/match arrays. Core identities keep their existing answers; unknown answers remain positional nulls. Inventories smaller than 16 retain the early original direct paths, including small warm queries.

| Contract | Implementation boundary and regression coverage |
| --- | --- |
| Exact UID and source order | Compare full integer UIDs after candidate selection; preserve first owner, shared diagnostic-name format, core names, duplicate input positions and original owner-array order. |
| Real vs synthetic ownership | Require `entry.apk != null`; public-only, installed-only and both-view cases are exercised independently. |
| Publication and removal | Borrow only the captured snapshot whose package/removal map identities remain current, and only when its removal map is empty. Pending removal retains admitted ownership until actual registry publication. |
| Special platform ordering | Visibility reserves the android entry's position. A real android APK with a non-system UID therefore retains the original registry-scan path. |
| Live authority | Only metadata is reused. Runtime grants/revocations, installer attribution, resources, certificate admission and existing permission decisions remain under their actual owners. |
| Caller isolation | Input UID arrays are not modified; result arrays are fresh; unrelated-user UID aliases are denied; replacement/reused UIDs cannot retain old owners. |
| Concurrency | A batch using a captured view/map has one coherent generation. Direct small paths retain their prior semantics. A blocked retired constructor cannot replace a new generation or block UID queries on the PMS monitor. |

The snapshot adds eight shallow bytes on this measured JVM layout (32 → 40). There is no new retained UID map. Query-local cold-batch arrays and the larger primitive table's construction cost are measured rather than omitted.

## Measurement method

The final warm experiment has 23 cases, two alternating A/B/B/A rounds per case and **four independent JVMs per variant/case**. Each JVM warms its actual workload and records seven correlated intervals. The table uses the median of the four per-JVM medians, divided by the number of completed operations. Seven intervals in one JVM are not presented as seven independent processes.

The final first-query experiment has nine cases and six A/B/B/A rounds: **12 independent first completed calls per variant/case**. Setup constructs the admitted metadata outside the timed interval. An existing-view case explicitly builds that view before measuring the first UID call. A cold-visibility case includes actual publication and view construction. These are first component calls in fresh host JVMs, not launch-to-frame, application startup or disk-cold measurements. The p95 values are descriptive interpolations from 12 observations; they do not qualify a production tail-latency budget.

Every measured operation calls the actual public PMS/framework entry point and checks its complete result. The permission case executes the actual permission policy for ACCESS_NETWORK_STATE; independent functional tests cover live CAMERA grant/revoke and denied UID aliases. The fixture constructs package metadata and supplies the existing declared native Binder/clock/framework-catalog boundaries. It does not claim APK parsing, Windows IPC, a physical Android oracle or actual ART Binder transport.

The host is Linux x86-64, Intel Xeon Platinum 8573C, with nine visible logical CPUs. The exact Temurin Java version and command lines are retained. Both variants use `-Xbatch`, a 256 MiB maximum heap and the same dependencies/fixture. Caller-thread CPU, caller allocation, wall time and process CPU are recorded. Short process-CPU intervals are visibly quantized; a recorded zero means below counter resolution, not zero work. Parallel GC/compiler work and scheduling can make caller CPU and wall/process observations differ.

### All warm completed-operation controls

“Rebuild visibility view” invalidates/reconstructs the actual view on every call after JVM warm-up. “No view” means UID queries never construct one. An equal-UID inventory is constructed metadata for the owner-list/group traversal boundary; these timings do not claim an APK admission workload.

| Completed operation | Packages | Batch input | Caller CPU ns, baseline → candidate | CPU change | Allocated bytes, baseline → candidate | Wall change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Name, existing view | 4,096 | 1 | 24,537.32 → 56.99 | -99.77% | 0.00 → 0.00 | -99.76% |
| Owner array, existing view | 4,096 | 1 | 26,712.69 → 64.30 | -99.76% | 120.00 → 24.00 | -99.76% |
| Permission, existing view | 4,096 | 1 | 24,760.52 → 102.36 | -99.59% | 112.00 → 40.00 | -99.59% |
| UID batch, existing view | 4,096 | 128 | 3,129,837.71 → 1,475.03 | -99.95% | 528.00 → 528.00 | -99.95% |
| UID batch, no view | 4,096 | 128 | 4,453,550.26 → 39,113.59 | -99.12% | 528.00 → 1,584.00 | -99.13% |
| All owners of one UID | 4,096 | 1 | 52,048.41 → 31,449.96 | -39.58% | 66,656.00 → 16,400.00 | -35.22% |
| Name, no view | 4,096 | 1 | 22,781.70 → 23,539.59 | +3.33% | 0.00 → 0.00 | +3.05% |
| Name, no view | 4 | 1 | 13.70 → 14.38 | +4.98% | 0.00 → 0.00 | +4.99% |
| Name, existing view | 4 | 1 | 12.95 → 14.39 | +11.18% | 0.00 → 0.00 | +11.29% |
| UID batch, no view | 4 | 4 | 49.23 → 45.40 | -7.79% | 32.00 → 32.00 | -7.21% |
| UID batch, no view | 16 | 16 | 401.09 → 394.91 | -1.54% | 80.00 → 80.00 | -11.30% |
| UID batch, no view | 32 | 16 | 735.91 → 643.45 | -12.56% | 80.00 → 240.00 | +14.30% |
| UID batch, no view | 16 | 128 | 2,741.65 → 2,777.24 | +1.30% | 528.00 → 528.00 | +3.03% |
| Repeated first UID, no view | 4,096 | 128 | 798.37 → 686.85 | -13.97% | 528.00 → 560.00 | -11.51% |
| System/shell batch, no view | 4,096 | 128 | 1,815.79 → 2,124.64 | +17.01% | 9,232.00 → 9,232.00 | +29.76% |
| Selective visibility | 2 | 1 | 109.99 → 103.94 | -5.50% | 56.00 → 24.00 | -9.95% |
| Selective visibility | 50 | 1 | 108.70 → 90.18 | -17.04% | 56.00 → 24.00 | -27.70% |
| Hidden target visibility | 4,096 | 1 | 319.86 → 193.75 | -39.43% | 78.15 → 46.15 | -49.53% |
| All-target visibility | 4,096 | 4,095 | 133,545.11 → 83,938.75 | -37.15% | 69,632.00 → 4,112.00 | -46.07% |
| Two equal-UID groups | 256 | 1 | 129,363.56 → 133,930.19 | +3.53% | 56.00 → 24.00 | +4.85% |
| Rebuild visibility view | 2 | 1 | 1,162.30 → 994.83 | -14.41% | 1,426.89 → 1,186.89 | -31.19% |
| Rebuild visibility view | 50 | 1 | 4,533.01 → 4,049.68 | -10.66% | 9,888.00 → 8,384.00 | -33.70% |
| Rebuild visibility view | 4,096 | 1 | 427,300.02 → 450,950.88 | +5.53% | 559,376.00 → 428,160.00 | -4.17% |

### First completed calls

| First completed operation | Packages / UID inputs | Wall ms, baseline → candidate | Wall change | Caller CPU change | Descriptive p95 wall ms, baseline → candidate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Name, no view | 1 / 1 | 0.123804 → 0.128438 | +3.74% | +3.85% | 0.162178 → 0.227975 |
| Name, no view | 32 / 1 | 0.125803 → 0.130074 | +3.40% | +3.36% | 0.194663 → 0.163635 |
| Name, existing view | 4,096 / 1 | 0.685516 → 0.031595 | -95.39% | -95.09% | 0.856203 → 0.045603 |
| UID batch, no view | 16 / 16 | 0.362772 → 0.352233 | -2.91% | +3.64% | 0.708143 → 0.492744 |
| UID batch, no view | 32 / 128 | 1.610210 → 0.560293 | -65.20% | +12.18% | 6.134434 → 0.756143 |
| UID batch, no view | 4,096 / 128 | 19.739728 → 3.319994 | -83.18% | -76.87% | 21.671599 → 5.237764 |
| Permission, no view | 4,096 / 1 | 28.596325 → 28.315953 | -0.98% | -5.22% | 36.699794 → 45.834329 |
| Rebuild visibility view | 2 / 1 | 17.543049 → 18.179075 | +3.63% | +0.91% | 20.008703 → 24.432508 |
| Rebuild visibility view | 4,096 / 1 | 52.349664 → 47.035576 | -10.15% | +12.59% | 57.964119 → 58.379337 |

The large cold batch's median wall interval is 19.740 → 3.320 ms, but this is not a universal cold-path improvement. First cold visibility construction has lower median wall time while caller CPU rises 12.59%. The first 32-package/128-UID batch similarly has lower wall time while caller CPU rises 12.18%. The one-package name and two-package view controls retain higher median wall times; several descriptive p95 tails also worsen. All observations stay in the release-budget discussion.

### Adverse controls and interpretation

- The tiny warm name control rises from 12.95 to 14.39 ns per completed operation, **+11.18%**, with no allocation change. The direct small loop is retained, but dispatch/index eligibility adds a measured small-path cost. Its absolute size does not authorize hiding the relative regression.
- A core-only 128-UID batch rises from 1.816 to 2.125 microseconds of caller CPU, **+17.01%**, with unchanged 9,232-byte allocation; wall time rises 29.76%. The broader cold-batch dispatch/compilation shape is a plausible explanation, not a demonstrated profiler diagnosis.
- At 32 packages and 16 UID inputs, caller CPU falls 12.56%, but wall time rises 14.30%, temporary allocation rises 80 → 240 bytes and the coarse process-CPU medians rise. This threshold case is an explicit CPU/allocation/scheduling tradeoff, not a universal batch win.
- Repeated 4,096-package visibility construction retains **+5.53% caller CPU** despite 4.17% lower wall time and 559,376 → 428,160 allocated bytes. The primitive table avoids boxed nodes but still performs its own probing and preparation. First-construction caller CPU is also adverse. Allocation/storage savings do not establish a release CPU budget.
- A cold repeated first UID uses 528 → 560 bytes while reducing caller CPU 13.97%. It avoids key/match arrays, but its actual iteration allocation remains measured.

No acceptance limit was added to turn these controls green. The bounded implementation is submitted with its evidence for a workload-specific release decision; significant controls above the epic's 5% criterion remain unresolved.

### Reported contention and retained earlier prototypes

During the final warm run, another agent reported an evidence-only Python enumeration taking approximately 3.8 seconds of one CPU. The conservative interval **11:49:30–11:50:30 UTC** potentially overlaps receipts 28–48: part of the warm bulk case, the cold bulk and broad owner-group cases, and one large cold-name process. The archive preserves the message-derived interval, all original logs and approximate process spans derived from log completion timestamps and recorded subprocess durations. These are not exact inner-query overlap times. No sample is discarded, replaced or silently treated as uncontended. No overlap was reported for the separate 216-process first-query run.

The archive also retains both rejected earlier prototypes and their actual source inputs:

- The first boxed-map reuse pilot gave large scan reductions but increased a warm 128-UID batch's allocation from 528 to 2,272 bytes; small/crossover regressions were also recorded. Replacing the existing group map removes that boxing rather than adding a second retained map.
- The first demand-grown primitive table reduced storage but increased the 4,096-package view-construction caller CPU by 35.86% in that pilot. The final table starts with a package-count size hint, uses the same integer spreading rule as the previous HashMap, and compacts shared groups after construction. The final remaining adverse construction observations above are still disclosed.

The earlier pilot percentages used their recorded sample aggregation; the final report consistently uses independent-process medians. Historical and final samples are never pooled into a single acceptance statistic.

## Structural storage, separately instrumented

A read-only Instrumentation agent measures shallow sizes of the **actual produced objects**, including the view, group table/arrays or HashMap nodes/keys, package-name map nodes, Entry objects, protected-broadcast set and predicate wrapper. No agent runs in timed JVMs. Shared package/parsed objects and strings, native heaps, temporary construction, private commit and total process memory are excluded. Fourteen structural probes are repeated in each final experiment with the same results.

| Actual metadata structure | Baseline bytes | Candidate bytes |
| --- | ---: | ---: |
| 4-package public view's group index | 384 | 184 |
| 4-package public view structure | 1,216 | 1,016 |
| 4,096 distinct-UID public view's group index | 229,536 | 65,592 |
| 4,096 distinct-UID public view structure | 557,696 | 393,752 |
| 4,096 equal-UID public view's group index | 32,976 | 120 |
| 4,096 equal-UID public view structure | 361,136 | 328,280 |
| Owning snapshot, shallow bytes | 32 | 40 |

These are structural overhead measurements, not a claim of 29% less application memory or attainment of the epic's process-tree memory target.

## Correctness, work and negative controls

The complete actual-source host suite passes **36,273 checks in each protected-broadcast mode**, plus **763 maintained intent contracts** and their existing work budgets. An independent reviewer replayed the same sources and results. The unchanged #345 production source passes **3,454 shared UID API/lifetime controls in each mode**; the new optimization's work budget intentionally fails on that unchanged source.

Coverage includes null/empty/unknown/core/full-user/negative/extreme UIDs, duplicate/reversed batches, first-owner and shared-name behavior, fresh arrays, 1,024 scattered UIDs plus second-user aliases, growth/collisions, both cached-view shapes, actual grant/revoke, pending removal, failed removal recovery, replacement/reuse, retired snapshot rejection, deterministic blocked-constructor publication, and 1,000 batch queries racing 400 whole-map publications. Actual UID/signature/admission/persistence tests also pass with the verified signing fixture and real filesystem, including unpublished/published interruption recovery.

Source-bound counters operate on the actual admitted registry's iterator only in the work fixture:

- A cold 128-UID query over 32 packages visits **32 registry records once**.
- 256 warm identity/public-permission operations visit **zero unrelated registry records**. The pre-existing permission-policy index is warmed before this interval; permission decisions are still evaluated live.

All six isolated **complete-production-source mutations** are rejected by their specific API assertion: removing exact UID filtering, borrowing a retired registry generation, dropping a pending owner, reversing first-owner order, skipping collision probing, and choosing the last uniform-batch owner. The unchanged baseline fails the cold registry-visit budget. No helper replacement is used to manufacture these failures; the source hashes, expected assertion and full logs are archived.

After composing main `fdf4861b`, the complete app and services Java/D8 builds pass, including 292 boot references with zero unresolved symbols. The same 36,273 × 2 checks and 763 intent contracts pass on the integrated app. The incoming actual F09 controller/policy/store suite passes **106 checks**. Current main's selected-source, shared-stdio qualification, supplier and 28 artifact-batch checks pass. The incoming producer-authentication and native/runtime gates remain enabled. Independent canonical-tree review confirms the integration preserves source and mode identities.

All current PR full-CI runs must be tied to their exact published heads. Local passes do not replace mandatory Windows/ART/Binder gates or grant full #56/#178 acceptance.

## Reproduction

Build the actual baseline app at the pinned #345 head and the candidate app with `app/build.sh`, using the recorded real framework/core dependency and the repository's R8/apksig inputs. Then run the complete existing production-backed gate:

```sh
python3 tools/test_package_visibility.py \
  --java-home "$UID_JDK" --framework-classes "$UID_FRAMEWORK" \
  --core-classes "$UID_CORE" --app-classes "$UID_CANDIDATE_APP" --apksig "$UID_APKSIG"

python3 tools/test_uid_lookup_mutations.py \
  --java-home "$UID_JDK" --framework-classes "$UID_FRAMEWORK" \
  --core-classes "$UID_CORE" --app-classes "$UID_CANDIDATE_APP" --apksig "$UID_APKSIG" \
  --baseline-ref d3f72de8a79e2446d41c4bf40824ebbcb42769b4 --output-dir /tmp/uid-mutations-new

python3 tools/benchmark_package_uid_lookup.py \
  --java-home "$UID_JDK" --framework-classes "$UID_FRAMEWORK" \
  --core-classes "$UID_CORE" --apksig "$UID_APKSIG" \
  --baseline-app-classes "$UID_BASELINE_APP" --candidate-app-classes "$UID_CANDIDATE_APP" \
  --baseline-ref d3f72de8a79e2446d41c4bf40824ebbcb42769b4 \
  --work-dir /tmp/uid-benchmark-new --output /tmp/uid-benchmark-new.json \
  --phase both --rounds 2 --first-rounds 6
```

Use fresh output paths: the driver rejects an existing result/work directory rather than overwriting prior evidence. Reproduction on a newer integrated application is a new experiment and must retain its own source/dependency hashes; the archived controlled data remain tied to their original dependencies. The archive also includes every final complete source input and command, all earlier pilot source inputs and the read-only storage probe. Framework/native boundaries, source changes, failures, allocation increases and slow observations must remain explicit.
