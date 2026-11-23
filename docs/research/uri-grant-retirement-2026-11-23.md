# URI grant retirement by package-publication identity

This leaf removes repeated scans of installed-package transient URI grants while
preserving the existing permission decisions and ownership lifecycle. On the
frozen host service workload, median caller CPU for an explicit read in a
128-entry table falls **61.69%**, and for a 512-entry table **83.20%**. A complete
512-URI owner creation, preparation, publication, readback and retirement cycle
uses **55.52%** less caller CPU.

The same fixed campaign also records costs that prevent a universal performance
acceptance claim: registry publication has **25.45% higher median caller CPU**
(10.70 → 13.43 µs), and the complete 128/512-URI cycles allocate **19.81% / 31.39%
more caller-thread bytes**. The first 512-URI cycle's elapsed p95 and the first
registry-publication p95 increase. These observations are retained below, with
all raw data, uncertainty and measurement boundaries. There is no new release
budget in this change. The statistically meaningful allocation increases and the
other adverse controls require an explicit budget under [#178](https://github.com/gesher-runtime/gesher/issues/178)
before release; this report does **not** establish that acceptance.

The work supports [#56](https://github.com/gesher-runtime/gesher/issues/56) F10/F11
ownership and T12 performance qualification. It does not close those roadmap
items, the overall package-metadata workstream, whole-application startup,
Windows/ART performance, remote Binder, pressure or soak qualification.

## Production change and scope

`PackageManagerService` gives each complete installed-registry publication a
fresh plain `Object` identity. Allocation occurs before replacing `mPackages`,
under the existing service monitor. The token carries no registry, APK,
permission or provider references. It is null before the first real publication.

Each existing ordered per-UID grant map becomes `GrantTable`, with one identity
reference and one hosted-source flag. After a complete stale-transient sweep, it
records the current publication identity. A later operation can omit that sweep
only when the non-null identity still matches and the table has never admitted
a hosted transient source. A hosted registration can change independently of the
installed registry; such a table permanently uses the old walk until the entire
table is removed. Persisted-only loads do not invent a transient hosted owner.

The optimization does not memoize permission verdicts. All existing live
participant, exact UID, installed-object, durable owner epoch, pending-removal,
provider, mode and permission checks remain in their actual call paths. The
prepared batch is still validated as a whole under PM → grant monitors before
insertion. Replacement with the same UID, removal/reinstallation, owner death,
force-stop, revocation, persistence and callback/unindexing ownership retain
their existing behavior. There is no additional monitor, background worker,
permission index, or persistent store.

The scope is compliant with the current `AGENTS.md` and `CONTRIBUTING.md`: the
application performs the same authorized URI operation, results and failures
remain real, and the tests' synthetic metadata/native identity adapter are
isolated from ordinary application execution. No capabilities or visible
desktop defaults change.

## Source identities and integration boundary

The measured baseline is commit `d3076fc4d29d102ab633299d4654e85a6b261f79`.
The frozen measured feature is local commit
`3a9fb1bf80c7446bef022aef68bbaee970deafdf`, tree
`9aa5097ef9f857e996746a02c326577d7b4b599e`.

| Measured production source | SHA-256 |
| --- | --- |
| PackageManagerService.java | `925e348c044ff5fec5f45fee121421dc5c1c7c55814f88cc066aaf1fa397f6f8` |
| UriGrants.java | `44bc8a52319fe4ab224b6d71a06e8aa63ef0ba558bd72df9ce05610b91c859ad` |
| ContentProviders.java | `a5d7477414b4c4c550169f0e0a3dd5c042dc48da7b6cf4ccc4b2c628b5b6894f` |
| UriPermissionModes.java | `155e0fe86459ad56e128c60d8ecb46a46430e09f44fe118d7128804c6ff8b83e` |

After the measurement sources were frozen, two local canonical merges incorporated
current prerequisites. `f5c3d2fef37c889cdc5673e850c7c8088cda2ddf` adds main
`52513f0fc3f5fc657138cf01429cf969b2f21649` (#297 and #409), with no Java or fixture
overlap. `eb49c574eabf176730feb26bdd01267a85e3a4b5` adds main
`5882d0a44765be56562ba25443b8572b6466712a`, preserving #387 camera metadata,
#325 provider authority selection, #287 persistent surfaces, #257 cancellation
and #309 inotify/fixture changes. The complete canonical tree is
`ce526ff81f1916d5ceba25a0647ca82e913190fe`.

The only feature/incoming overlap is PackageManagerService. Removing its exact
three token-related additions recovers incoming main's whole file byte for
byte; UriGrants remains the measured `44bc8a52…` source. All 4,665 canonical paths,
modes and blobs match. The merged PMS hash is
`bce9808bf50af72f1b27471cb27854d3703471299b1c557baabf3b8eca386a36`.
The newer provider implementation is **separately qualified**, not represented
as the source of the older frozen timing data. The measured and integrated
build manifests are retained separately.

## Complete execution boundary

The ordinary host caller compiles the complete production PM, URI-grant,
provider and permission-mode sources against the actual app classes and pinned
framework build. The maintained JNI adapter supplies native initialization and
caller identity; it does not implement a permission or grant algorithm. The
actual `ActivityManagerService.checkUriPermission` path executes for each timed
explicit read. There is no reflective invocation adapter or reference oracle
inside that query clock.

| Workload | Work inside each operation |
| --- | --- |
| exact1/8/128/512 | One actual explicit permission check, rotating through the seeded installed-owner table. |
| missing128 | One complete denied explicit permission check in a 128-entry table. |
| prefix128 | One authorized child-URI check through a 128-entry prefix-grant table. |
| hosted1 | One hosted-provider grant check; the conservative original sweep remains. |
| mixed_hosted128 | One hosted grant check in a table containing 128 installed grants plus the hosted grant; original sweep remains. |
| owned_cycle1/128/512 | Real Binder owner creation, Intent/ClipData preparation, complete grant publication, readback of every URI, and actual owner retirement. |
| publish_registry | Real `publishPrepared` package publication plus execution and retirement of its one actual queued permission-policy callback. The existing grant remains live and is checked after the measured intervals. |
| empty_control | Complete denied explicit permission check with no grant table. |

Fixture construction and initial seed grants are outside the clock. Final
fixture cleanup is also outside it, but is mandatory and fails the process if
strong owner memberships or UID tables survive. Every query checks its result;
every batch checks every URI; every process checks the expected final table and
actual owner retirement. A failure cannot become a fast sample. The first
observation is the **first operation after fixture setup**, not full app startup,
installation, a cold OS cache, or a completely uninitialized JVM.

Caller CPU and allocated bytes use HotSpot's ThreadMXBean. Elapsed time uses
System.nanoTime. Process CPU is also retained, but has a coarse process clock
and includes VM/compiler threads; it is not the primary first-call metric.
Caller allocation is neither retained heap nor whole-process private commit.
No counter, mutation, javaagent or compilation trace is present in ordinary
timing classes. The source/class/app-JAR digests are checked before and after
each block.

## Fixed final campaign

The schedule was declared at `2026-11-23T21:26:02.463774Z`: six complete ABBA
blocks, all 13 cases in every block, order baseline/candidate/candidate/baseline.
There are **312 independent JVM executions and 1,248 observations**. Each JVM
records one first call, performs its fixed warmup, then records three fixed
steady intervals. All observations remain present. There are no retries,
selected samples, post-result tuning or additional favorable-run selection.

Ordinary query cases use 131,072 warmup operations and three 32,768-operation
intervals. Prefix queries use 32,768 and three 8,192-operation intervals.
Owned cycles use 8,192/2,048 operations for size 1, 128/32 for size 128, and 32/8
for size 512. Registry publication uses 8,192 warmup and three 2,048-operation
intervals. One JVM is an independent statistical unit; its three intervals are
summed and divided by their total operation count before comparing JVMs.

All three other lanes explicitly stayed CPU-idle during each block; bounded
build/review gaps occurred between blocks. The exact intervals are retained:

| Block | Started UTC | Finished UTC | JVMs / observations |
| --- | --- | --- | --- |
| 1 | 2026-11-23T21:29:41.295444+00:00 | 2026-11-23T21:30:28.234198+00:00 | 52 / 208 |
| 2 | 2026-11-23T21:35:17.074128+00:00 | 2026-11-23T21:36:02.457529+00:00 | 52 / 208 |
| 3 | 2026-11-23T21:40:28.612611+00:00 | 2026-11-23T21:41:16.916390+00:00 | 52 / 208 |
| 4 | 2026-11-23T21:51:37.183752+00:00 | 2026-11-23T21:52:41.300870+00:00 | 52 / 208 |
| 5 | 2026-11-23T21:59:25.903782+00:00 | 2026-11-23T22:00:29.492386+00:00 | 52 / 208 |
| 6 | 2026-11-23T22:07:16.027810+00:00 | 2026-11-23T22:08:16.581105+00:00 | 52 / 208 |

### Steady complete-call medians

These are medians of the 12 per-JVM costs per variant/case, not medians of 36
correlated intervals. Negative changes are reductions; positive changes are
costs. The JSON also retains every process-CPU and descriptive p95 observation.

| Complete operation | Caller CPU µs/op, baseline → candidate | CPU change | Elapsed µs/op | Elapsed change | Caller bytes/op | Allocation change |
| --- | --- | --- | --- | --- | --- | --- |
| exact1 | 0.63 → 0.53 | -16.73% | 0.63 → 0.53 | -16.68% | 384.00 → 384.00 | +0.00% |
| exact8 | 0.60 → 0.48 | -19.29% | 0.60 → 0.48 | -19.35% | 384.00 → 384.00 | +0.00% |
| exact128 | 1.54 → 0.59 | -61.69% | 1.54 → 0.59 | -61.70% | 424.00 → 384.00 | -9.43% |
| exact512 | 5.39 → 0.91 | -83.20% | 5.39 → 0.91 | -83.17% | 384.00 → 384.73 | +0.19% |
| missing128 | 3.36 → 1.98 | -41.10% | 3.45 → 2.20 | -36.28% | 2,376.00 → 2,376.00 | +0.00% |
| prefix128 | 6.57 → 5.31 | -19.14% | 6.91 → 5.76 | -16.55% | 15,764.00 → 15,752.00 | -0.08% |
| hosted1 | 0.57 → 0.54 | -5.59% | 0.57 → 0.54 | -5.58% | 376.00 → 376.00 | +0.00% |
| mixed_hosted128 | 1.63 → 1.69 | +3.38% | 1.66 → 1.69 | +1.84% | 416.00 → 416.00 | +0.00% |
| owned_cycle1 | 20.85 → 18.42 | -11.67% | 23.97 → 20.49 | -14.52% | 9,974.63 → 10,051.33 | +0.77% |
| owned_cycle128 | 891.20 → 732.20 | -17.84% | 911.72 → 751.06 | -17.62% | 747,651.92 → 895,748.08 | +19.81% |
| owned_cycle512 | 7,802.61 → 3,470.99 | -55.52% | 8,038.29 → 3,594.19 | -55.29% | 2,770,173.17 → 3,639,696.33 | +31.39% |
| publish_registry | 10.70 → 13.43 | +25.45% | 11.70 → 14.30 | +22.21% | 4,533.90 → 4,663.35 | +2.86% |
| empty_control | 2.41 → 2.29 | -5.15% | 2.45 → 2.42 | -1.33% | 2,128.00 → 2,128.00 | +0.00% |

### Paired block estimates and uncertainty

Each ABBA block has two independent JVMs per variant. The paired estimate pools
the six block means; its exact empirical bootstrap enumerates all 6^6 = 46,656
resamples of those six blocks. Six blocks provide limited uncertainty
resolution. The intervals are descriptive evidence for these runs, not a
universal Windows guarantee and not grounds to discard an adverse median.

| Operation | Pooled CPU change | Six-block paired bootstrap 95% interval | Pooled elapsed change | Pooled allocation change |
| --- | --- | --- | --- | --- |
| exact1 | -8.79% | -28.46% to +18.05% | -8.69% | -0.52% |
| exact8 | -24.27% | -34.88% to -11.86% | -23.81% | -0.31% |
| exact128 | -63.93% | -69.93% to -56.32% | -64.09% | -8.42% |
| exact512 | -82.91% | -85.11% to -79.79% | -82.96% | -1.54% |
| missing128 | -35.57% | -51.45% to -16.12% | -33.89% | -0.08% |
| prefix128 | -18.88% | -34.45% to -4.37% | -17.39% | -0.03% |
| hosted1 | +0.54% | -14.86% to +20.26% | +2.13% | -1.50% |
| mixed_hosted128 | +5.80% | -1.29% to +14.20% | +5.47% | -1.20% |
| owned_cycle1 | -4.26% | -29.22% to +17.95% | -0.54% | +1.04% |
| owned_cycle128 | -18.24% | -33.70% to +1.47% | -17.51% | +15.11% |
| owned_cycle512 | -53.60% | -56.93% to -49.86% | -52.07% | +26.90% |
| publish_registry | +15.07% | -3.49% to +33.62% | +14.89% | +3.71% |
| empty_control | -11.41% | -58.20% to +84.13% | -10.62% | -0.29% |

### First operations and tails

There are only 12 first observations per variant/case. These p95 values are
linear-interpolated descriptive quantiles of that small sample, not a validated
production tail-latency bound. The larger first publication and 512-owner-cycle
tails remain visible even where their medians improve.

| Operation | First caller CPU ms | First elapsed ms | Descriptive first elapsed p95 ms, baseline → candidate |
| --- | --- | --- | --- |
| exact1 | 0.70 → 0.62 | 0.69 → 0.61 | 1.04 → 0.78 |
| exact8 | 0.65 → 0.68 | 0.67 → 0.67 | 2.02 → 0.96 |
| exact128 | 0.76 → 0.76 | 0.75 → 0.76 | 2.51 → 1.15 |
| exact512 | 0.79 → 0.62 | 0.78 → 0.62 | 1.22 → 1.08 |
| missing128 | 0.33 → 0.32 | 0.32 → 0.32 | 0.50 → 0.54 |
| prefix128 | 1.20 → 0.95 | 1.21 → 0.95 | 2.07 → 1.38 |
| hosted1 | 0.67 → 0.71 | 0.65 → 0.71 | 0.87 → 1.00 |
| mixed_hosted128 | 0.86 → 0.67 | 0.85 → 0.71 | 2.50 → 1.22 |
| owned_cycle1 | 26.78 → 28.30 | 27.08 → 28.96 | 48.48 → 41.62 |
| owned_cycle128 | 59.01 → 58.13 | 59.99 → 60.16 | 92.83 → 91.85 |
| owned_cycle512 | 136.30 → 92.98 | 147.01 → 102.12 | 236.34 → 281.64 |
| publish_registry | 29.11 → 27.21 | 31.90 → 27.33 | 59.07 → 124.15 |
| empty_control | 18.91 → 18.26 | 19.22 → 18.94 | 27.10 → 29.45 |

### Adverse controls and release acceptance

* **Registry publication:** median caller CPU grows 25.45% (about 2.72 µs per
  complete publication), and elapsed time grows 22.21%. The six-block pooled
  CPU increase is 15.07%, with a −3.49% to +33.62% interval. First elapsed p95
  grows from 59.07 to 124.15 ms. The source adds one token allocation, but the
  measurements do not establish that this allocation explains all observed
  CPU or scheduling variation. No causal conclusion is inferred from an
  unchanged-looking control.
* **Complete-cycle allocation:** 128 URIs rise from about 747,652 to 895,748
  caller bytes/cycle, and 512 URIs from 2,770,173 to 3,639,696. Their pooled
  increases are 15.11% and 26.90%, with allocation intervals excluding zero.
  These increases are not the retained token size, and no escape-analysis or
  GC cause is asserted without a separate diagnostic. They remain a material
  measured tradeoff despite lower complete-cycle CPU.
* **Hosted fallback:** mixed-hosted median CPU grows 3.38%; its pooled CPU is
  +5.80% (−1.29% to +14.20%). The original sweep is intentionally preserved.
  Hosted1's first median and first p95 increase. The small/warm confidence
  intervals cannot establish zero overhead.
* **Small complete cycle:** owned_cycle1 median improves, but its first median
  CPU grows about 1.52 ms and first elapsed about 1.88 ms. Its pooled steady CPU
  interval includes regressions. Exact1 and empty-control pooled intervals
  are also broad; their favorable median is not a universal tiny-path claim.
* **Large first tail:** owned_cycle512 first elapsed p95 is 236.34 → 281.64 ms,
  despite its much better median. Missing/empty first tails also increase.

The change demonstrably eliminates repeated retirement work and substantially
reduces the larger target CPU paths. This does not establish #178's release
budget. Representative Windows/ART measurements, an explicit CPU/allocation/tail
budget, combined process memory and pressure/soak qualification remain open.
No acceptance threshold was invented to turn these controls green.

## Work, storage and ownership qualification

Separate instrumented production runs count retirement visits; these classes
are never timed. Identical complete outputs and real cleanup are required.

| Workload | Baseline visits | Candidate visits |
| --- | ---: | ---: |
| 128 checks in a 128-grant installed table | 16,384 | 0 |
| First check after package replacement, 128 grants | 128 | 128 |
| Complete publication of an owned 128-URI batch | 8,128 | 0 |
| 128 checks in a mixed 129-grant hosted table | 16,512 | 16,512 |

The existing per-UID ordered map remains the sole authority. A separate
Instrumentation.getObjectSize run follows two actual 128-grant tables through
real package publication, one table refresh, another publication and table
retirement. It validates that every generation is exactly plain Object and
that the distinct identity count is at most one per table plus the current
registry.

| Phase | Baseline token bytes | Candidate identities / shallow bytes |
| --- | ---: | ---: |
| Both tables share the current publication | 0 | 1 / 16 |
| One table refreshed after publication | 0 | 2 / 32 |
| Two retained table generations plus current registry | 0 | 3 / 48 |
| Both tables retired | 0 | 1 / 16 |

In this HotSpot layout PMS is 120 bytes in both variants, and the two maps total
128 shallow bytes in both variants because layout padding absorbs the added
fields. That is an observed VM layout, not a claim that the fields cost nothing
on ART/Windows. Source storage is one reference in PMS, one reference and one
boolean per table, plus a plain identity per publication while reachable.
The tokens retain no package graph. Whole-process retained/peak memory and GC
pause effects were not measured by this shallow observation.

## Correctness, negative controls and integrated checks

The maintained complete entry passes on unchanged baseline and candidate:
154 URI checks, 28 Intent checks, 99 persistable checks, 27,326 comparisons with
the [pinned unmodified Android permission-mode implementation](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/services/core/java/com/android/server/uri/), and 6,211 new
retirement/lifetime checks. The new corpus includes exact full UID and shared
UID separation; wrong-user checks; same-UID source/target replacement;
pending-removal/abort; revocation between preparation/publication; durable epoch
change; null-generation construction; remove/readd and table reuse; hosted
authority mutation and rollback; mixed hosted/installed tables; complete
256-URI owner cleanup; deterministic owner retirement waiting on PM; and a
blocked real metadata getter proving publication/query synchronization.

Four separately compiled production mutations fail their intended assertions:
no generation rotation, one global table generation, ignored hosted fallback,
and skipping the null generation. Their original output, source receipts and
failure identities remain in the evidence. No assertion or deadline is relaxed.

The Windows services gate retains the existing actual Binder transport tests
and adds the direct 6,211-check retirement entry. The reusable framework gate
builds the complete production host entry and its separate work-count corpus.
The current `ci.yml` invokes its local reusable framework, so these gates are
part of the same submitted source tree.

Full app Java/D8 and full services Java/D8 builds pass for the measured feature.
Current-main composition also rebuilds the full app and reruns the complete
URI/lifetime corpus, services build and strict selected-source/supplier/vector
preflights. Exact commands, outcomes and hash receipts are recorded in the
machine-readable report; pending items must not be represented as completed.

Independent source review replayed the complete maintained contracts on the
frozen production and confirmed the PM → URI monitor order, non-retaining
token, insertion validation, hosted fallback and live-policy checks. A separate
current-main composition review covers the new provider authority implementation.
The independent review receipts distinguish supplied gates from independently
executed checks. Full latest-head fork CI and actual Windows/ART acceptance are
reported separately on the PR; local host success is not that result.

## Earlier evidence retained

The initial fixed 52-JVM/208-record pilot is preserved, including CPU increases
for missing128 (+8.71%), hosted1 (+14.78%), mixed-hosted (+12.59%) and owned1
(+7.41%), plus the first owned1 cost. Its shorter warmup showed substantial
within-JVM movement. A bounded four-JVM unchanged-binary PrintCompilation
diagnostic showed actual service C1/C2 compilation during/after those intervals.
That justified predeclaring a longer fixed warmup; it does not prove the cause
of every adverse pilot observation. Diagnostic clocks are not ordinary timing
evidence. No production source changed between pilot and final. All pilot
sources/classes, ordinary correctness smokes, diagnostics and final count
smokes are retained rather than silently replaced.

## Reproduction and evidence

Use a JDK 21, the declared framework/core class artifact and apksig dependency.
The complete production entry is `tools/test_uri_retirement.py`; it supports an
unchanged `--source-ref`, separate `--count-work`, `--storage` and the four exact
negative controls. Ordinary timings use `--compile-only` once, then the frozen
class trees, without any of those instrumentation options.

The companion JSON binds the source commits, complete class/app-JAR hashes,
predeclaration, block intervals, every raw stdout digest, all first/steady
statistics and validation receipts. The evidence archive retains the exact
sources, raw logs, compiled-class receipts and analysis scripts with an embedded
member inventory. Baseline and current integration outputs remain distinct.
The archive also includes a Git bundle of the exact three local source commits,
with public main5882 as its prerequisite. After fetching that public main,
`git bundle verify` and `git bundle unbundle` can recover the named local objects;
the corresponding published commits retain their exact reviewed trees and
ordered parent roles. Report-only finalization does not change measured code.
No network timing, physical hardware, remote Binder, ART, Windows runtime,
whole-app TTI, total process-tree memory or eight-hour soak result is claimed.

The shared framework/core compile dependency is the previously downloaded
successful main artifact from [run 45094093107](https://github.com/gesher-runtime/gesher/actions/runs/45094093107),
archive SHA-256 `8e27ecee656f984b7745333bcfb765d0e723dcb38084e05223e5dde7b44da631`.
After the campaign, all 31,777 framework and 5,879 core members were rechecked
byte-for-byte against their original ZIPs. That is an explicitly post-campaign
dependency check, not a new per-sample framework observer. It is an older
framework artifact, while all app/changed service sources were rebuilt from
their declared baseline/candidate/current integration trees. The exact host
JDK, CPU model, platform and dependency digests are in the archived environment
and dependency receipts; the shared Linux host is not dedicated Windows
performance hardware.
