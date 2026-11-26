# Query-local component metadata generation

This is a component leaf for [#56 T12](https://github.com/gesher-runtime/gesher/issues/56)
and [#178 JAVA-06](https://github.com/gesher-runtime/gesher/issues/178). It removes
repeated full-family metadata generation inside an activity, receiver or service
intent query. It does not complete package parsing, provider registration,
whole-application startup or the broader performance acceptance workstreams.

Baseline: `d38b5549dc3963f8d491e9205f9b2743bb6d3745`. Final `ApkPackage.java`
SHA-256: `bbee0fecea2627787ba821244b2fdacbfe913f23315a20f5981d9587f6d3c3c6`.
The [JSON companion](component-query-metadata-2026-11-23.json) retains every
measurement, compared source hash, exact command and rejected prototype.
Repeated command/source manifests use lossless `data_references` entries;
their reference IDs are SHA-256 of sorted-key, compact JSON values.

## Work removed and ownership preserved

The existing query first matches parsed intent filters. For every match it then
calls the corresponding single-component getter. Each getter invokes the real
framework `PackageInfoCommonUtils.generate` for the entire component family and
selects one component afterward. With C declarations and M matching filters,
this constructs approximately M × C component records, including siblings that
are immediately discarded. The package-level intent index already avoids
unrelated packages; it does not remove this repeated work inside a selected APK.

For more than one match and at least eight family declarations, the candidate
generates one family and indexes its first remaining declaration by name. It
walks the original match list, preserving declaration order, first matching
filter, priority, duplicate-result multiplicity and the single getter's first
name after direct-boot/enabled filtering. Every returned component is a fresh
`ActivityInfo` or `ServiceInfo`. Zero/single-match queries and smaller families
retain their original direct single-getter loops.

The first result takes the newly generated family's `ApplicationInfo`. Each
additional result receives another independently generated `ApplicationInfo`,
with component-generation flags removed through the existing application-info
path. This still executes the real framework generator and installed metadata
processing for each result. In particular, it preserves live shared-library
resolution and independent mutable files, lists and dependency graphs. It does
not add a manual clone implementation for arbitrary framework/library state.

The framework activity copy constructor shares its generated embedding-cert
set. The candidate explicitly copies that set using the framework setter, so
duplicate component names cannot make caller mutations cross results. Parsed
metadata, window-layout and attribution-array sharing that already existed in
the single getter is unchanged; this is not a claim of deep-copying all parsed
objects.

One immutable enabled-state value is captured for each actual batch and passed
to both family and per-result application-info generation. A deterministic
mid-generation transition therefore cannot produce a component from one state
and its application metadata from another. The next query reads the new state.
The direct tiny/single path retains the existing per-result state read, including
its behavior when state changes between two result generations. Public single
getters and provider registration retain their existing semantics. The provider
call only passes its fresh state to the common generator explicitly.

The temporary name map and generated metadata are local to one call. There is
no persistent result cache, new package-manager lock, retained application
decision or package-specific path. Public PM visibility, user filtering and
caller checks remain in their existing wrappers.

## Complete-query measurements

JDK 21.0.13 Temurin, Linux x86-64 host; the actual complete `ApkPackage` and PMS
sources are compiled against the same prepared Android framework/core and app
dependencies. Constructed immutable manifests exercise the full production
framework generator. The timed manifests do not override metadata getters or
replace the generator. Each result's component identity, UID and resolved
filter is consumed and checked.

Each scenario runs in a fresh JVM in baseline/candidate/candidate/baseline
order. After warmup, each JVM supplies seven samples: 14 samples per revision
per scenario, 252 steady samples altogether. `System.gc()` occurs outside the
measured interval in both revisions. The counters are calling-thread CPU,
calling-thread allocated bytes and query elapsed time. They do not measure
whole-process CPU, retained heap, private commit or application launch time.
Samples from one JVM are correlated; these medians are component evidence, not
a hardware release certificate.

| Scenario | Queries per sample | CPU before → after, ms | Allocated bytes before → after | CPU change |
| --- | ---: | ---: | ---: | ---: |
| 128 activities, all match | 500 | 496.654 → 48.799 | 1,931,816,000 → 163,664,000 | −90.17% |
| 128 services, all match | 500 | 395.529 → 42.387 | 883,240,000 → 142,160,000 | −89.28% |
| 128 receivers, all match | 500 | 470.249 → 52.194 | 1,931,816,000 → 163,664,000 | −88.90% |
| 2 activities, both match | 500,000 | 696.535 → 728.510 | 2,506,000,280 → 2,514,000,280 | +4.59% |
| 128 activities, one match | 10,000 | 104.791 → 108.672 | 333,520,000 → 333,920,000 | +3.70% |
| 128 activities, four match | 5,000 | 176.449 → 73.471 | 618,760,000 → 233,300,000 | −58.36% |
| 128 activities, no matching filter | 100,000 | 277.911 → 259.551 | 308,400,280 → 308,400,280 | −6.61% |
| 128 matching activities, all disabled | 500 | 473.430 → 6.384 | 1,911,858,000 → 20,204,000 | −98.65% |
| 8 activities, all match | 20,000 | 171.628 → 127.964 | 602,800,188 → 411,200,000 | −25.44% |

Broad activity/receiver allocation falls 91.53%; service allocation falls 83.90%.
The disabled case still matches the filters and generates the family before
applying actual installed enabled state; an empty result does not skip the
contract. The two warm control regressions are reported explicitly. Their
allocated-byte changes are +0.32% and +0.12%, respectively. No measured steady
median exceeds a 5% CPU regression, but this does not establish absence of a
regression on other hardware or workloads.

### First-query cost and explicit tradeoff

The first real query is also recorded before warmup in every fresh JVM. This
boundary includes lazy framework class initialization reached by that query,
and excludes JVM startup and manifest construction. One of the initial two
tiny-candidate observations took 67.954 ms CPU, versus roughly 21–26 ms for the
other three observations. It remains in the raw data. Those two observations
per revision are not enough to make a cold-start claim.

A focused follow-up uses six ABBA rounds for each of two family sizes: 12 fresh
JVM observations per revision per size, with the same final production and
fixture source hashes:

| First-ever query | CPU before → after, ms | Elapsed before → after, ms | Allocated bytes before → after |
| --- | ---: | ---: | ---: |
| 2 matching activities | 27.162 → 26.435 | 27.583 → 26.818 | 1,620,936 → 1,620,784 |
| 8 matching activities | 22.944 → 26.961 | 23.518 → 27.396 | 1,653,720 → 1,656,808 |

The eight-component cold CPU median is **17.51% higher**, or **4.017 ms**. This
cost must not be hidden behind the broad steady gains. Paired round means show
+12.89%; a deterministic 20,000-resample bootstrap across six round pairs gives
approximately +0.66% to +26.62%. The sample supports a cold cost, with substantial
uncertainty about its size. No unmeasured JVM mechanism is asserted as its cause.

The implementation decision retains the eight-component path's 25.44% steady
benefit and the large-family savings at that measured first-query cost. It does
not pass a blanket 5% cold-regression criterion. An explicit cold/startup budget
must be agreed and qualified on representative Windows hardware before release;
no post-hoc threshold is used here to declare release acceptance. This is not a
production deadline, universal startup improvement, or completed #178 acceptance.
Dedicated Windows/ART and whole-application cold/warm budgets remain required.

### Rejected intermediate measurements

The initial version batched tiny families and routed direct queries through an
extra helper. It improved broad activity CPU 87.15%, but regressed tiny CPU
51.12%, single-match CPU 16.40% and misses 5.65%; it was rejected. Restoring the
direct loops and an eight-declaration threshold removed the extra tiny map and
copy work, but an added `packageInfo` forwarding method still accompanied a
10.73% tiny regression in that run. The final code removes that unnecessary
forwarding level. A longer isolated tiny follow-up then measured −7.88%, and
the final complete run measured +4.59%. Both are retained: the difference shows
host/JVM variation and does not support a universal tiny-path speed claim.

A more conservative floor of 16 was also built and measured, with all other
production bytes unchanged. It passed the same 7,040 metadata checks, 564
expanded state-transition checks and full app/services builds. Its 128-component
workloads still reduced CPU 87.7–88.6%, and its cold controls improved in that
run. However, its now-direct eight-component warm queries regressed 13.86%
(162.900 → 185.479 ms per 20,000 queries), with unchanged allocated bytes. The
floor of 16 was rejected because it gave up the existing repeated-query saving
without establishing universally lower cold/warm cost. Its 280 steady and 96
cold observations are retained separately, including all favorable results.

The source audit finds no extra counting traversal or helper call on that
direct path: both revisions perform the same matching pass, and the extra
selection checks are constant-time list-size reads. The observations do not
establish a particular JVM compilation mechanism. A separate final-floor-eight
cold experiment at 16/32 declarations measured +0.150/+1.557 ms median CPU;
paired uncertainty spans improvement and regression. These observations are
also retained rather than extrapolating the eight-component cold ratio to all
family sizes.

All intermediate source hashes, interval sizes and raw samples are segregated
from final-source measurements in the JSON. They are not substituted for the
final nine-scenario results. The committed reproduction wrapper was also
executed with one cold ABBA round to verify its compilation, source manifests,
fresh-JVM commands and complete receipts; those smoke values remain separately
labeled and are not the primary performance results.

## Contracts and negative control

- 7,040 full-field comparisons against the actual public single-component
  getters, across activity/receiver/service families and 7/8-declaration paths.
  The unchanged baseline passes the same static comparisons. Coverage includes
  disabled/direct-boot filtering, first-name duplicates, first-filter priority,
  metadata/query flags, explicit/empty paths, order and caller mutation isolation.
  Fresh `ApplicationInfo.createTimestamp` values are checked for valid uptime,
  rather than incorrectly requiring separately constructed objects to have the
  same construction instant.
- Application/library isolation uses actual `SharedLibraryGraph.apply`: mutable
  arrays, lists and dependency nodes do not cross result/query ownership, while
  shared dependencies inside one graph retain their identity. The input graph
  is an explicit constructed fixture; this is not an installed XML/jar test.
- 372 deterministic enabled-state checks across 1/2/7/8 components, all three
  families and disabled-user/disabled-until-used transitions. They verify both
  the batch snapshot and the inherited per-result tiny behavior.
- A counted `ParsedPackage` returns its actual lists unchanged. For 64 components
  and eight queries, all three candidate families perform 528 family-list reads.
  The unchanged baseline performs 33,288 activity-list reads and fails the same
  strict 528-read budget with the intended assertion. No metadata generator is
  replaced in either run.
- The existing 763 intent candidate contracts and work budgets pass, along with
  79 visibility checks in each GMS mode through the maintained entry point.
- Complete app Java/D8 and complete services Java/D8 builds pass. The boot link
  check finds 292 framework references and zero unresolved references.
- Independent review of the final source finds no scope/correctness blocker and
  independently replays the metadata, state, intent and family-work checks.

The default host package-visibility gate invokes these new checks through the
existing intent candidate driver. Required Windows track 98 invokes the same
service fixture and now requires the metadata/work/coherence result markers.
Host inputs include the prepared main `d2ebbdfd` framework/core artifact; the
complete candidate production app is rebuilt. Actual Windows/ART, parsed APK
installation and remote Binder execution remain full-CI/product validation,
not results inferred from host mocks.

The leaf was then rebased onto upstream `5aa6b39027e9c38de2781ff9cda066418d25b1ba`,
including the newly merged exact-alarm and DNS work. This leaves the entire CI
configuration unchanged from current main, including its published framework
recipe `fe42fc3d4854141df94cd1e05cfbd10c4a686315` and new DNS/exact-alarm gates.
No old workflow recipe is reintroduced. `ApkPackage` and the timed fixture bytes
remain exactly the measured versions. Upstream's PMS addition routes exact-alarm
settings outside the timed `ApkPackage` boundary; current-main integration is
validated by new complete app/services Java+D8 builds, the maintained visibility/
intent entry point (79 checks per mode, 763 intent checks, 7,040 metadata checks,
372 coherence checks and all three 528-read budgets), and 414 exact-alarm
contracts. These current-main results and their source/log hashes are recorded
separately in the JSON. They are not presented as a new whole-product timing run.

## Android contract sources

The baseline uses the real framework generator, and this leaf preserves its
flags and filtering rather than introducing a replacement generator:

- [Android 16 r4 PackageInfoCommonUtils](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/pm/parsing/PackageInfoCommonUtils.java)
  generates complete requested families and their application metadata.
- [ActivityInfo](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/ActivityInfo.java)
  and [ComponentInfo](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/ComponentInfo.java)
  define the copy behavior and explicit embedding-certificate setter.
- [System-server PackageInfoUtils](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/parsing/PackageInfoUtils.java)
  and [ComponentResolver](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/resolution/ComponentResolver.java)
  provide the per-result metadata/ownership comparison. Their internal indexing
  design is not required by Gesher's [scope policy](../../CONTRIBUTING.md).

## Reproduction

Use the prepared real framework/core/app dependencies and JDK 21. `$BASE_APP`
is the complete app classes JAR used as the common compilation dependency for
both revisions; use current main or the complete candidate JAR when reproducing
after the exact-alarm merge. The runner recompiles the complete baseline and
candidate PMS/ApkPackage sources ahead of that shared dependency. The recorded
timing run used the complete d38b5549 dependency and its contemporaneous PMS;
the timed `ApkPackage` source is unchanged by the later integration. `$CANDIDATE_APP`
is the complete candidate classes JAR for current-head correctness. Set `$JDK`,
`$FRAMEWORK`, `$CORE`, `$APKSIG` to their local paths. Build ordinary app/services
artifacts with the maintained build scripts.

```sh
python3 tools/test_package_visibility.py --java-home "$JDK" --framework-classes "$FRAMEWORK" --core-classes "$CORE" --app-classes "$CANDIDATE_APP" --apksig "$APKSIG"
python3 tools/test_intent_candidate_index.py --java-home "$JDK" --framework-classes "$FRAMEWORK" --core-classes "$CORE" --app-classes "$BASE_APP" --apksig "$APKSIG" --source-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --metadata-case contracts
python3 tools/test_intent_candidate_index.py --java-home "$JDK" --framework-classes "$FRAMEWORK" --core-classes "$CORE" --app-classes "$BASE_APP" --apksig "$APKSIG" --source-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --metadata-case work
python3 tools/benchmark_component_metadata.py --java-home "$JDK" --framework-classes "$FRAMEWORK" --core-classes "$CORE" --app-classes "$BASE_APP" --apksig "$APKSIG" --baseline-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --output steady.json
python3 tools/benchmark_component_metadata.py --java-home "$JDK" --framework-classes "$FRAMEWORK" --core-classes "$CORE" --app-classes "$BASE_APP" --apksig "$APKSIG" --baseline-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --cold-rounds 6 --output cold.json
python3 tools/benchmark_component_metadata.py --java-home "$JDK" --framework-classes "$FRAMEWORK" --core-classes "$CORE" --app-classes "$BASE_APP" --apksig "$APKSIG" --baseline-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --cold-rounds 6 --cold-components 16 32 --output larger-cold.json
```

The third command is the negative control and must fail with
`Query repeated full-family metadata generation: 33288`. A compile failure or
unrelated exception is not a successful negative control. The benchmark compiles
both complete production sources, writes their source manifests, runs each
scenario in a fresh JVM and accepts only complete checked-query receipts.
