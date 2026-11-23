# JAVA-08: WebView geometry scratch and publication

This is an allocation and reentrancy contribution to [issue #178](https://github.com/gesher-runtime/gesher/issues/178).
It does not complete JAVA-08 or the performance epic. Dynamic traversal, polygon
approximation, Windows/ART validation and end-to-end performance acceptance remain.

## Source and overlap

The implementation starts from main `8b316aebf09ab46d5407e955bf86ed748a1e739f`
and includes the independently reviewed shared CI foundation
`42417047c95321c4e2257b05bdf5900c11c10a74`, tree
`d8b5ea533a4b06fd7ab5b5ef11c2decffab23b5c` ([#226](https://github.com/gesher-runtime/gesher/pull/226)).
The 19 open PRs inspected before implementation had no changed WebView, View,
ViewRoot, WindowSession, geometry or HWUI paths. The measurement changes from
#230 are not included.

Read the repository's [scope policy](../../CONTRIBUTING.md). The Android-visible
operation is still placement of the WebView2 child window at the Android View's
location and visible rectangle, with the existing shapes cut out for views drawn
above it and the existing outline clipping. No application input, callback,
capability, error or workflow is substituted. Geometry uses the same Android
View/Outline APIs and the same JNI shape record format.

## Production change and ownership

[`GesherWebViewProvider`](../../app/java/com/gesher/app/webview/GesherWebViewProvider.java)
previously allocated a ten-element placement array on every update, an exact-size
hole copy before comparing it, a location array for every shape and an empty hole
array on hidden updates. It also reused mutable Outline/Rect state across
application callbacks without isolating nested collection.

The provider now compares placement scalars with the retained last placement. A
collection frame owns location, outline, visible-rectangle and path-bounds scratch
plus a growable packed-shape buffer. The comparison reads only its valid prefix.
Only different shape contents produce an exact-size immutable JNI snapshot;
placement-only updates retain the old snapshot. Empty shape sets share one empty
array and are still passed as null to JNI.

Scratch belongs to each WebView provider, on its existing Looper. Two collection
frames are cached: the ordinary call and one nested call. Deeper reentry uses
temporary frames. A cached nested frame whose integer shape capacity exceeds
4,096 entries (16 KiB) is discarded on return. The ordinary shape buffer retains
the existing grow-to-fit policy; this change does not impose a new polygon limit
or claim a total Java/native path-memory budget. The extra retained nested integer
buffer has that explicit cap; the last submitted shape snapshot has its original
lifetime and exact size.

An outline provider may call back into this WebView, another WebView, or its
lifecycle. Every pass observes the completed-collection counter at entry. A valid
unchanged result or a returned native submission advances that counter. An outer
pass observing a completed nested result returns without overwriting it; it also
rechecks the host, attached state and destroyed state before publication. A failed
nested attempt does not advance the counter, so the application's outline recovery
or the existing rectangle fallback can still finish a valid outer collection.
Nothing rolls the counter back when a later enclosing pass fails: an already
completed deeper result remains authoritative. `finally` releases the depth even
when a View getter throws. Reentry into a different WebView uses that provider's
own scratch.

Independent QA found the failed-attempt defect in the initial local candidate
`dcd8189ea0036450c56243fdfdd554de3c32908e`: counting admitted calls could indefinitely
suppress publication when every outline callback attempted a failing nested
update. The correction records completion, including the unchanged fast path.
The regression suite covers both application-caught and production-caught nested
getter failures, subsequent movement, unchanged nested completion, and a deeper
changed or unchanged completion followed by failure of its enclosing collection.

The native ownership boundary is unchanged:
[`WebView2Host_setBounds`](../../compat/webview/webview_jni.cc) copies Java records
before returning, and [`Host::SetBounds`](../../compat/webview/webview_host.cc)
captures the decoded shapes by value for its posted WebView-thread work. Collection
scratch is never published to that boundary.

## Preserved traversal and known limits

Every visible update still traverses the relevant ancestors and siblings. It
continues to observe Z/order, alpha, visibility, clipping, location, size,
background and mutable outline changes even without layout/scroll notification.
The existing 32-hole and depth-16 descendant limits are unchanged. Reentrant
passes are isolated, not suppressed by a dirty flag.

`Path.approximate(0.5f)` still allocates its result and still runs for every path
outline. Mutable application paths/providers have no new reference-identity
cache. Application callbacks and Android View/graphics internals may allocate as
well. Large/deep exceptional reentry and changed or growing shape data can
allocate. Therefore the zero-allocation result below is for the stated geometry
fixtures, not a claim that every possible Android screen allocates nothing.

The existing outline conversion does not apply view scale/rotation to its packed
outline coordinates. This contribution preserves that implementation and does
not establish that existing transformation handling is fully Android-correct.
Real rendering, pixel clipping, hit testing, occlusion, animations and insets with
multiple WebViews still require Windows/ART/WebView2 qualification.

No subtree cache or cadence change is introduced. Node visits are expected to be
unchanged; no UI-thread p95, CPU, RAM, GC, process-tree or whole-product improvement
is claimed from these host checks.

## Reproduction and evidence

Run the new default CI contract:

```sh
python3 tools/test_webview_geometry.py --java-home "$JAVA_HOME" --sanitize
```

For a checkout containing the baseline object, compare production output:

```sh
python3 tools/test_webview_geometry.py --java-home "$JAVA_HOME" \
  --compare-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f --sanitize
python3 tools/test_webview_geometry.py --java-home "$JAVA_HOME" \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f --case measurement --measure
```

The fixture extracts all geometry fields and methods without rewriting them.
Its declared Android boundary supplies view state, visible rectangles and outline
callbacks. Collection, comparisons, packing, scratch ownership and reentry execute
production Java. Emitted frames then execute the exact production JNI decoder
with declared JNI-array and Host-submission boundaries.

| Check | Local result |
|---|---|
| Behavior, two WebViews, dynamic same-provider outlines, hierarchy/order/Z/alpha/visibility/clip changes, polygon mutation/fallback, 16/32 limits and immutable published snapshots | 2,715 checks passed |
| Actual callback reentry, depth eight, cross-WebView calls, getter failure recovery, detach and destroy | 17 checks passed |
| Failed nested attempts, valid outer recovery, unchanged completion, deeper success followed by enclosing failure and later recovery | 59 checks passed |
| Non-reentrant production output against the main baseline | All 670 submitted frames byte-identical; transcript SHA-256 `4a965788643d7b4773f56bb0a2690dcc11c47250a3b4ddd4f9a7388e85216022` |
| Exact JNI decoding of those Java-generated frames, with ASan/UBSan | 20,808 checks passed |
| Unmodified old-source reentry control | Fails because the older outer pass overwrites the nested publication |
| Unmodified old-source allocation gate | Fails on the first 5,000-frame case with 560,000 allocated bytes |
| Initial local candidate `dcd8189e`, new recovery contract | Fails because a failed nested attempt suppresses valid initial outer placement |
| Existing owned WebView response contracts with restored framework classes | 422 behavior, 331 concurrency and 4 measurement checks passed |
| Existing WebView boundary dispatch contracts | 32 checks passed |
| Complete app javac/D8 build with Android libcore module | Passed against historical source-bound compile inputs; not current Windows runtime evidence |
| Windows x64 static-loader and DLL-loader WebView2 probes | Both cross-compiled; not executed on Windows locally |
| Actual WebView JNI source, Windows x64 shared-bridge mode | Cross-compiled with warnings treated as errors; not executed on Windows locally |
| Repository compatibility parser tests; CI YAML/Python parsing; whitespace | 18 parser tests passed; parsing and whitespace checks passed |

The allocation fixture runs HotSpot in interpreter mode, uses the thread allocated
byte counter, and warms each case for 1,000 frames before counting 5,000 frames.
This avoids scalar replacement hiding baseline arrays and tiered compilation or
deoptimization adding unrelated objects to the measured interval. The native
submission adapter does not record frames in the allocation interval. This is a
Java allocation contract, not a latency benchmark.

| Fixture, 5,000 measured frames | Baseline Java bytes | Candidate Java bytes |
|---|---:|---:|
| Visible, no shapes | 560,000 | 0 |
| 32 rectangles | 8,880,000 | 0 |
| 32 rounded outlines | 8,880,000 | 0 |
| Placement changes with identical shapes | 8,880,000 | 0 |
| Hidden view | 560,000 | 0 |
| Stable geometry with one nested outline update | Not used as an equivalent baseline due to its reentry defect | 0 |

Local LeakSanitizer cannot operate in the container's ptrace environment. The
successful local sanitizer run used `ASAN_OPTIONS=detect_leaks=0`; ASan and UBSan
remained enabled. The CI command retains its default leak checks. Windows
execution, ART allocations, complete product gates and real workload measurements
remain pending on the published candidate.
