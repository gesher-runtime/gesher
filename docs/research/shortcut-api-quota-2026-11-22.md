# Shortcut API accounting and owned foreground exemptions

Scope: the shortcut API accounting leaf of #56 F51/T12. Baseline:
`972c2b42b40c4ac799e147f5e352d07ba615d0fb`. This change does not close F51's
static, pinned, cached or share-target requirements, or F12's general process
importance implementation. It does not introduce a shortcut presentation path.

## Android contract

The reference is the immutable `android-16.0.0_r4` tag. The accompanying
[JSON receipt](shortcut-api-quota-2026-11-22.json) records primary URLs and SHA256
hashes of the exact downloaded source files, changed production/test sources,
build inputs and result logs.

| Reference | Relevant behavior |
| --- | --- |
| [ShortcutService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ShortcutService.java) | Default 10 updates per 24 hours; public validation order; system/shell/root reset authority; signature reset permission; global clock phase; UID foreground threshold at BOUND_FOREGROUND_SERVICE. |
| [ShortcutPackage](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ShortcutPackage.java) and [ShortcutUser](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ShortcutUser.java) | Per-package counts; current foreground and intervening foreground visits; unlimited permission; locale and clock resets; persistence. |
| [OomAdjusterImpl](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/OomAdjusterImpl.java) | Actual per-process service and counted-provider state propagation; binding flag distinctions; external provider handles only reach IMPORTANT_FOREGROUND, which is outside the shortcut exemption threshold. |
| [ProcessRecord](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/ProcessRecord.java) and [WindowProcessController](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/wm/WindowProcessController.java) | Shown-UI history starts at actual activity launch; HOME is an actual process identity, not a package-name heuristic. |
| [ContentProviderHelper](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/am/ContentProviderHelper.java) | Asynchronous MIME lookup uses an external null-token reference, not a counted application-thread connection. |

The default interval is taken from the pinned implementation, rather than
substituting an example interval from documentation. Counted calls are
`setDynamicShortcuts`, `addDynamicShortcuts` and `updateShortcuts`. Background
calls exceeding the limit return false. Existing removal, enable/disable and
push operations remain outside this counter. The number of dynamic shortcuts
allowed by the existing implementation is a separate limit.

Caller/publisher identity, user, input count and null entries are checked before
quota admission. Label, intent and other late payload validation happens after
admission: a late-invalid call consumes a slot, while an already-throttled call
returns false before those late checks. The actual unlimited and reset
permissions are checked through PermissionPolicy; requesting a signature
permission in a manifest is not enough to obtain it.

The global reset calculation deliberately preserves the pinned clock-phase
quirk. Starting at noon on day 0 and next accessing at 00:01 on day 2 computes
a noon-day-2 anchor, temporarily ahead of the clock. The following access takes
the reference's clock-rewind path. The fixture checks both accesses and their
different next-reset values. It also covers the source's literal clock-validity
constant, early negative clocks and saturating overflow.

## Ownership and resource design

`ShortcutQuota` holds per-package counters identified by name, UID and first
installation time. Updates preserve a valid installation's counters; replacement
by another installation cannot inherit them. Committed non-replacing removal
retires absent owners. A failed removal, pending removal or delayed event for a
new installation cannot erase its current counter. State loading validates the
complete detached record set before publication; truncation, invalid counts,
duplicate current owners and trailing data cannot publish a partial prefix.

The GSQ1 file persists counters, the global reset anchor and locale. The existing
PolicyStateWriter coalesces immutable snapshots, writes and syncs a temporary
file, then atomically replaces the old file. Failed writes do not refund quota.
Explicit privileged activation and shutdown flush the writer. Denied calls with
unchanged state perform no publication or quota write. There is no periodic
quota polling thread or per-query miss cache.

Foreground eligibility comes from currently owned process records. Real activity
and foreground-service visits are recorded even when an application returns to
background before its next shortcut call. The shortcut threshold also includes
processes bound to TOP/FGS owners, so enforcing a quota required preserving those
exemptions:

- ActiveServices publishes immutable service/connection metadata on actual
  ownership changes. The client is its authenticated ProcessRecord and code
  generation, not any foreground process sharing the UID. The local standalone
  path retains a client only when the actual UID, code, current process and
  application thread match; a trusted package claim alone is insufficient.
- ContentProviders publishes active stable/unstable counted connections and
  retires them at final release, disable or process death. Mixed provider and
  service chains use the reference's different propagation rules. External
  provider/MIME references are excluded because their source-defined state 6
  does not satisfy the shortcut threshold 5; no unowned permanent root is added.
- Every eligibility evaluation samples current process membership, code,
  activity/FGS state, component state, package removal and connection lifetime.
  A work queue follows only reachable improving states. Cycles without a current
  foreground root cannot preserve an exemption. Immutable topology is reused;
  foreground results are recomputed on every query.
- Temporary membership/state maps are cleared in `finally`, including after
  failure. Their backing storage can be reused, and shrinks when the live process
  registry falls sufficiently below its previous size. It retains no process
  references or foreground authority between evaluations. Numeric UID observation
  history only detects real visits; it never answers eligibility queries.
- `WAIVE_PRIORITY`, `NOT_FOREGROUND` and `IMPORTANT_BACKGROUND` restrict
  propagation. `NOT_VISIBLE` and `NOT_PERCEPTIBLE` change OOM adjustment and do
  not automatically disqualify the process-state exemption. Shown-UI
  `ALLOW_OOM_MANAGEMENT`, actual system-client exceptions and awake-only
  bindings follow the pinned source. A bound persistent host cannot retransmit
  the system server's stronger adjustment exception.
- PowerManager's real interactive transitions refresh both sleep and resume
  observations. The regression takes one eligible call, sleeps and resumes
  without a shortcut query, unbinds, then checks the restored allowance. A
  duplicate resume event does not manufacture a visit. The old resume-only
  implementation fails this same fixture.

No new ATM-to-controller monitor nesting is needed: controllers publish topology
under their own monitors and invoke the observer afterward. Eligibility runs
under ATM against immutable snapshots. Foreground visits take only quota state,
not the synchronized shortcut API monitor. Snapshot persistence and explicit
flushes do not run under controller monitors. These states are used only for
shortcut accounting; they are not reported as a general OOM/AppOps capability.

## Actual overload work

The same host workload invokes the actual service and AppShell disk publication
10,000 times with one valid, empty shortcut snapshot from a background publisher.
The baseline helper compiles unchanged historical production sources. The
candidate runs the complete current controller/quota path. No quota, publication
or file-I/O implementation is replaced. JFR observes actual JVM `FileWrite`
events, including the final durability flush; shell presentation is disabled.

| Result | Baseline | Candidate |
| --- | ---: | ---: |
| Accepted calls | 10,000 | 10 |
| Throttled calls returning false | 0 | 9,990 |
| Dynamic metadata FileWrite events | 20,000 | 20 |
| Dynamic metadata bytes | 3,140,000 | 3,140 |
| Quota FileWrite events / bytes | 0 / 0 | 1 / 71 |

This is **99.9% fewer dynamic metadata write events** in an overload/admission
workload. It is not a same-work latency comparison or an application-wide
speedup claim. The JSON preserves raw elapsed times, but the headline is the
source-defined rejection of work that should not have been admitted.

## Foreground graph cost

A separate workload installs 1,001 actual fixture ProcessRecords and 1,001
service connections: a chain plus a cycle. It calls the public shortcut service,
performs actual bind/unbind and activity-state transitions, and counts the real
connection predicates without replacing their result. The graph topology is
unchanged during query stages. These are HotSpot thread CPU/allocation samples,
not ART performance thresholds.

| Stage | Operations | Connection predicate calls | Allocated bytes | Thread CPU |
| --- | ---: | ---: | ---: | ---: |
| Background denied publications | 10,000 | 0 | 3,514,784 | 141.1 ms |
| Foreground allowance queries | 10,000 | 10,010,000 | 409,198,696 | 4,010.4 ms |
| Foreground visits | 32 | 32,032 | 1,835,016 | 21.3 ms |
| Root connection retirement | 1 | 2 | 380,440 | 1.5 ms |
| Denied publications after retirement | 10,000 | 0 | 6,758,040 | 227.3 ms |
| Root connection republication | 1 | 2,004 | 855,464 | 3.6 ms |

The foreground traversal is a material correctness cost: about 40.9 KB and
0.401 ms per query in this deliberately populated graph. Background calls still
scan current process roots, but visit no connection edges when none is reachable.
Topology publication copies owned metadata on bind/unbind changes. The receipt's
connection dimension describes the initial graph; root removal leaves 1,000
connections until republication. No stale foreground cache is used to hide these
costs. The default gate uses a smaller 65-process/65-connection graph and asserts
exact work counts, retirement, no topology rebuild on queries and scratch cleanup.

## Validation and CI binding

The final source passed, sequentially:

- Full app Java and DEX build against real framework/core classes; full services
  Java and DEX build. The local framework contains 31,777 classes; compatibility
  generation resolved 16 declarations from the supplied pinned source subset.
- Existing `test_shortcut_updates.py` entry point: 52 update checks, then 1,048
  quota contracts, actual 10,000-call file-work gate and the default graph gates.
- Service rebinding 431; activity lifecycle 389; AppOps 489; package-enabled
  lifecycle 343 in each GMS mode, provider declarations and authority index 32;
  power wake-lock 98.
- Exact unchanged-baseline and candidate file-work measurements, plus the larger
  process graph above. A temporary old resume-only PowerManager fails the new
  sleep/resume fixture; the corrected production passes it.
- Isolated composition with #260's nine feature files at
  `cdd9e3ace0ac9a111d77880caf46ab745bdc8171`, excluding its immutable workflow pins:
  clean merge, 173 foreground-service permission/admission checks and all 1,048
  quota checks. The JSON records the resulting composition tree.
- Independent peer source review and replay of the final quota/controller/power
  source, including the sleep/resume correction, found no remaining blocker.

The already-executed candidate `test_shortcut_updates.py` helper invokes the new
gate and propagates errors. This avoids an unbound step in a branch-local copy
of the pinned framework recipe. The maintained Windows `DynamicShortcutTest`
invokes the same contracts with real Parcel transport; its full services DEX
compiled locally. Actual Windows/ART execution has not been performed here.

The host fixtures use constructed parsed/signed package metadata and declared
native Binder identity/clock/AssetManager boundaries. Real framework objects,
PermissionPolicy, controller ownership, service APIs, persistence and file writes
execute. Controlled PowerManager events test its actual handler, not native
Windows suspend delivery. These results do not establish Windows shell latency,
real application startup/frame budgets, ARM translation cost or full F12 behavior.

## Reproduction

### Publication refresh against current main

Before publication, the original reviewed feature commit
`3f275050ba05cd16426ed3c330a684410a01b7b3` was merged with upstream
`452372561e0a50c63f072a5ae1e7ca5aa81354b8`. This brings in main's framework
recipe pin, TCP release-context repair and #283 document/photo-picker behavior.
The merge was automatic; no conflict was resolved by hand. The complete feature
patch across app, tests and tools has the same stable patch ID before and after
the merge: `86091a21edd31a276c381b52830a6f2e6faf8459`.

Only ActivityTaskManagerService changes among the quota helper's ten compiled
production sources, from the additive upstream picker changes. Its refreshed
SHA256 is `b21f61b2738ce440ddd6d845a186ab8fb9c8be10879e9aee7eaa026a0f0f2ba0`.
The other nine source hashes and every F51 fixture/helper remain unchanged.
The JSON preserves the original measurement hashes and records the refresh
separately, so the historical CPU/allocation samples are not relabeled.

The refreshed tree passed a new full app Java/DEX build followed by a full
services Java/DEX build. With that freshly built app jar, the maintained shortcut
entry point passed 52 existing and 1,048 quota checks, the exact 10,000-call
file-work bounds and the 65-process graph controls. The affected activity gate
passed 389 checks; the upstream SAF gate passed 799 checks, 14 detached-picker
ownership checks and all three grant-restart phases. These remain host tests
with the declared native boundaries; no new Windows/ART execution is claimed.

### Commands

Provide matching framework/core/app classes and JDK through the usual helper
arguments. `$APP` must be freshly built from the candidate source; `$CORE` is the
Android core class directory, not the host JDK.

```sh
python3 tools/test_shortcut_updates.py --java-home "$JDK" \
  --framework-classes "$FRAMEWORK" --core-classes "$CORE" \
  --app-classes "$APP" --apksig-jar "$APKSIG"
python3 tools/test_shortcut_quota.py --case work \
  --source-ref 972c2b42b40c4ac799e147f5e352d07ba615d0fb \
  --expected-published 10000 --file-events baseline.jfr \
  --java-home "$JDK" --framework-classes "$FRAMEWORK" \
  --core-classes "$CORE" --app-classes "$APP" --apksig-jar "$APKSIG"
python3 tools/test_shortcut_quota.py --case work --file-events candidate.jfr \
  --java-home "$JDK" --framework-classes "$FRAMEWORK" \
  --core-classes "$CORE" --app-classes "$APP" --apksig-jar "$APKSIG"
python3 tools/test_shortcut_quota.py --case graph --graph-nodes 1000 --graph-queries 10000 \
  --java-home "$JDK" --framework-classes "$FRAMEWORK" \
  --core-classes "$CORE" --app-classes "$APP" --apksig-jar "$APKSIG"
```
