# Installed intent candidates: production work measurements

Scope: #56 T12, with F25/F49 resolution contracts; #178 JAVA-06 candidate-index
leaf. Baseline: `bf7c6e2f1adac284bd00971734f7b50c63d72a94`, which includes the
separate provider-authority index from #272.

## Behavior and ownership

The ordinary PMS activity, service, receiver and provider-intent queries visited
every installed package. Explicit provider queries also generated provider
metadata for unrelated packages. Implicit queries repeatedly inspected unrelated
declaration lists and filters.

Named-package and explicit-component queries now select the installed record
directly. Unconstrained queries use a separate lazy action-to-all-owner candidate
snapshot for each component family. Installed order is retained, and repeated
actions within a package do not duplicate its query. The first query of a family
builds that family's snapshot; explicit queries build none.

Actual `ApkPackage` and Android `IntentFilter` matching still determine the
results. MIME types, resolved-type overrides, schemes, authorities, paths,
categories, default-only and direct-boot flags are evaluated on every query.
Null-action queries retain every package with a filter in that family, including
packages containing only zero-action filters. Explicit components require no
filter. Browser, service and hosted-document fallbacks keep their existing code.

No enabled-state, visibility, permission, result object or arbitrary query miss
is cached. Query execution remains under the existing package publication monitor,
so enabled-state batches cannot interleave with a multi-package result. Each
candidate list/map is immutable and bounded by installed declarations. Publication
immediately releases the old intent-index reference alongside the authority-index
reference; the exact package-map identity also guards lazy rebuilding.

The public visibility wrapper, activity-options resolution and metadata generation
within a matching package remain separate costs. This change does not complete
all JAVA-06 work or the broader F25/F49/T12 acceptance criteria.

## Measured production work

The fixture invokes the actual PMS query methods, `ApkPackage` resolvers, Android
intent matching and framework metadata generators. Constructed packages contain
one component per family and one distinct action per package. Queries target the
last installed package. Counters observe `Map.values()` iteration and actual
parsed declaration-list getters while returning their original values.

Every table entry covers **32 queries**. The raw source identities and all four
families' results are in `intent-candidate-index-2026-11-22.json`.

| Packages | Registry visits, baseline → indexed | Activity/service/receiver hit declaration reads | Provider-intent hit declaration reads |
| ---: | ---: | ---: | ---: |
| 1 | 32 → 0 | 96 → 96 | 96 → 96 |
| 50 | 1,600 → 0 | 1,664 → 96 | 4,800 → 96 |
| 500 | 16,000 → 0 | 16,064 → 96 | 48,000 → 96 |
| 1,000 | 32,000 → 0 | 32,064 → 96 | 96,000 → 96 |

Registry visits fall to zero for warm implicit hits/misses, explicit components
and package-constrained queries in all four families. At 1,000 packages, implicit
hits perform **99.70% fewer declaration reads** for activities/services/receivers
and **99.90% fewer** for providers.

Missing actions read zero installed declaration lists after warmup, versus
32,000 for activities/services/receivers and 96,000 for providers at 1,000
packages. The existing synthetic service fallback still runs when applicable;
the counters cover installed package records. Explicit provider queries drop
from 64,000 declaration reads to 64. Explicit queries of other families and
package-constrained metadata work retain their existing target-only counts.

Cold construction traverses the installed registry once per first-used family.
At 1/50/500/1,000 packages, candidate first-hit declaration reads are
4/53/503/1,003. Activity/service/receiver baselines are 3/52/502/1,002, so their
first hit adds one list read. Provider baselines are 3/150/1,500/3,000. Null-action
queries and actions shared by many packages still visit every possible owner.

These are production operation counts. Whole-application latency, Windows CPU,
RSS, startup time and hardware power were not measured.

## Validation and reproduction

- New contract fixture: **763 candidate checks / 751 baseline checks**. Twelve
  index-specific lifetime/bound/immutability assertions run only on the candidate.
- Identical canonical-scan parity matrix on both versions, using actual Android
  matching, including zero-action-only and filterless packages, distinct family
  actions, MIME/URI/category/default/boot flags and mutable result isolation.
- Live component/application overrides, pending removal, package reordering,
  replacement UID/action, uninstall/reinstall and 2,000 distinct misses.
- Three concurrent readers issue 2,400 queries while 200 package generations are
  published. Every query observes a complete valid owner generation.
- Actual CI visibility entry point: **79 existing checks in each GMS mode**, then
  the new fixture and all sixteen family/package-count work gates.
- Existing enabled-state suite: **214 checks plus persistence write/read in each
  GMS mode**. Existing searchable suites: **52 and 10 checks**.
- Full application Java/DEX and services Java/DEX builds pass against the real
  Android core/framework classes. The local build's available CompatChanges
  source catalogue contains three declarations; this is compilation evidence,
  without a complete catalogue qualification.

```sh
python3 tools/test_intent_candidate_index.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" --apksig "$APKSIG_JAR"
python3 tools/test_intent_candidate_index.py \
  --java-home "$JAVA_HOME" --framework-classes "$FRAMEWORK_CLASSES" \
  --core-classes "$CORE_CLASSES" --app-classes "$APP_CLASSES" --apksig "$APKSIG_JAR" \
  --source-ref bf7c6e2f1adac284bd00971734f7b50c63d72a94
```

The immutable framework workflow already invokes candidate-head
`tools/test_package_visibility.py`, whose default run now executes this fixture
with the same framework/core/app dependencies. Windows track 98 runs the shared
Android fixture. Windows/ART results must be attributed to the submitted head;
the local host runs use declared JNI initialization boundaries and constructed
manifests. They do not execute APK installation, Binder transport or an Android
device comparison.

Open #249 at `188c2cace33b3fc2d62165310994d7e92f88cf0a` separately adds a UID and
permission catalogue index. Its actual PMS diff has no intent-query optimization.
Integration must retain `packageSnapshot()`, both candidate-index retirements and
`mPermissions.invalidatePackageIndex()` in the shared publication helper. This PR
does not include #249 or its permission-policy changes.
