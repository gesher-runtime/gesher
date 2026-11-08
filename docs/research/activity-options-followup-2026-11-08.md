# Activity option queries — 2026-11-08

This follow-up audits `IPackageManager.queryIntentActivityOptions` at PR #8
commit `79567dadaacefd15983e9230bfd80290d6cd52f9` against the repository's pinned
`android-16.0.0_r4` sources. It addresses this API's missing operation, with the
existing resolver limitations stated below.

## Confirmed defect and effect on applications

`PackageManagerService` inherited `IPackageManager.Default`'s null result for this
operation. Both public `ApplicationPackageManager.queryIntentActivityOptions`
overloads convert that result into an empty list, even when installed activities
match the request. This is lost functionality rather than a demonstrated null
pointer exception in the public wrapper.

A concrete framework consumer is `MenuBuilder.addIntentOptions`: it uses these
results to create menu items, selects the original specific Intent using
`specificIndex`, assigns the resolved component, and loads its label and icon.
The missing query therefore suppresses real available actions. Apps can also use
the returned options directly for their own selection and launch flows.

Sources:

- [ApplicationPackageManager: both overloads and null-to-empty conversion](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ApplicationPackageManager.java)
- [PackageManager: public activity-options contract](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/PackageManager.java)
- [MenuBuilder: addIntentOptions](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/com/android/internal/view/menu/MenuBuilder.java)

## Implemented behavior

The service now takes one snapshot of its installed package inventory, excluding
pending removals, and delegates this operation to `ActivityOptionsResolver`.
The helper uses actual `ApkPackage` metadata, parsed activities and IntentFilters.
It does not add a synthetic resolver activity or return an empty list as a
substitute for resolution. Existing ordinary activity startup remains unchanged.

| Part of the operation | Implemented behavior |
| --- | --- |
| Generic matches | Real action, category, MIME type, URI scheme/authority/path and relative-URI-filter matching through the pinned framework's `IntentFilter`. |
| Filter selection | Package queries retain the first eligible matching manifest filter. Unrestricted queries retain the first match from Android's MIME/action/scheme lookup order, including exact MIME before wildcard filters. A package restriction inside a selector still uses unrestricted lookup cuts, as `ComputerEngine` chooses the query path before selector substitution. |
| Result order | Priority, preferred order, default status, match value, system status and package-name order, with stable ties. Ordinary positive activity priorities and privileged protected-action priorities are capped at zero, matching the supported installed-state policy. |
| Specific requests | Explicit targets use installed activity metadata; implicit targets resolve actual candidates. Supplied MIME types are respected. Specific results precede the generic tail, retaining the original array index after null or unresolved entries are skipped. |
| Deduplication | Specifics remove matching components and relevant action duplicates from the generic tail. Remaining generic results remove later duplicates of actions other than the originally requested action. |
| Caller | The first matching caller component is removed, following the pinned implementation. |
| Query flags | `MATCH_DEFAULT_ONLY`, `MATCH_DISABLED_COMPONENTS`, direct-boot filters, `MATCH_SYSTEM_ONLY`, metadata flags and `GET_RESOLVED_FILTER` use real current state. Unknown users return no matches. `MATCH_DISABLED_UNTIL_USED_COMPONENTS` alone does not override manifest-disabled state. Instant-caller visibility bits are cleared for the supported full-app caller model, as Android clears them for ordinary callers. |
| Metadata | Returned activity identity/paths, default status, match, system state, literal/resource labels, icon fields and owner user handle come from the actual matched component/filter. `preferredOrder` remains zero, as it does in the pinned activity resolver. |
| Ownership | Returned results and filters are fresh. Filter copies also detach mutable relative URI groups and nested extra values. Queries do not rewrite supplied Intents or registered manifest filters. |
| Host browser | The existing Windows URL-dispatch adapter remains queryable through its actual supported filter. An installed browser package pending removal does not resurrect the fallback adapter within the same query. |

Two surprising upstream behaviors are deliberately preserved and tested rather
than silently changed: repeated specifics targeting one component remain separate
entries, and an explicit specific can reuse the first removed generic result even
when that result was removed for a shared action rather than component identity.
Caller removal consequently removes only the first occurrence when repeated
specifics target the caller. These details come from the pinned implementation,
not an interpretation of the more general public documentation.

The specific-deduplication loop also guards a null resolved filter. An explicit
generic query legitimately returns a result without a filter; pairing it with a
different specific action must not dereference that null value.

Primary implementation sources:

- [ResolveIntentHelper: queryIntentActivityOptionsInternal and chooseBestActivity](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/ResolveIntentHelper.java)
- [ComputerEngine: queryIntentActivitiesInternal and updateFlagsForResolve](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/ComputerEngine.java)
- [ComponentResolver: result metadata, priority adjustment and comparator](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/resolution/ComponentResolver.java)
- [IntentResolver: lookup cuts and first-result component deduplication](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/IntentResolver.java)
- [PackageUserStateUtils: enabled and direct-boot matching](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/pkg/PackageUserStateUtils.java)
- [IntentFilter: matching and copy ownership](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/IntentFilter.java)
- [SaferIntentUtils: caller/target compatibility enforcement outside the options algorithm](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/pm/SaferIntentUtils.java)

## Validation and its boundaries

`tools/test_activity_options.py` recompiles the actual production service/helper
and runs the real framework Java classes on JDK 21. The final focused host run
passed **107 checks**, covering populated results, order and priority, filter
labels/icons, index preservation, the action/component deduplication rules,
caller removal, null filters, MIME lookup order, selectors, enabled/direct-boot/
system flags, install removal and result ownership. The same fixture compiled
with the pre-change service at the audit-base commit fails at the first populated
query because the inherited method returns null. An empty replacement would also
fail the subsequent exact activity-list expectations.

Independent review extracted the actual pinned options-combination method and
compared **600 varied requests**, with **610 input/state/edge assertions** passing
on the final helper. That oracle intentionally shares the raw candidate-query
helper: it independently checks combination/order/index/filter projection, not
600 independent parser or base-resolver implementations. Separate source review
and expected-label probes identified and verified the normal-caller visibility
flag correction and the selector-package MIME-cut correction. The latter probe
reproduced the wrong wildcard label on the earlier helper and passed on the fix.

The host test constructs real `PackageImpl` and parsed-component objects and
places their `ApkPackage` records in a real service inventory. It uses `Unsafe`
only to omit unrelated installer/permission-service construction. Native-boundary
adapters provide host arrays, clock/logging and pinned build properties; they do
not implement resolution, rewrite Android business classes, or test ART/Binder.

Reproduction with the verified build inputs used for the audit:

```sh
python3 tools/test_activity_options.py \
  --java-home /path/to/jdk-21 \
  --framework-classes /path/to/framework-classes.zip \
  --core-classes /path/to/coremod \
  --app-classes /path/to/gesher-app-classes.jar \
  --apksig /path/to/apksig.jar
```

Add `--source-ref 79567dadaacefd15983e9230bfd80290d6cd52f9` for the expected failing
negative control. Host output is diagnostic evidence, not a Windows runtime pass.

`ActivityOptionsContractTest` also has a separate ART/Windows entry point taking
the existing real `components.apk`. It parses that APK, runs the actual service
through the generated AIDL proxy/StandIn/Parcel path, and exercises both public
manager overloads, specific indices, flags and unknown-user handling. This path
is compiled against the actual Android core/framework at Java target 17; runtime
execution remains a separate Windows CI gate, not a claimed result of the host
test.
An independent final compilation of the frozen service, helper and complete
AIDL/public-manager fixture passed using JDK 21 and the actual Android core at
target 17. `git diff --check` also passed.

### Focused Windows bootstrap correction

On published `223d4b3b`, the actual ART invocation in
[Windows 2022 job 128649121906](https://github.com/gesher-runtime/gesher/actions/runs/42453120565/job/128649121906)
printed `ActivityOptionsContractTest framework passed: 108 checks`. That count
includes its main-entry argument assertion. It then failed before the public
manager queries: constructing the real `ContextImpl` tried to obtain a display
from an uninitialized external service manager (`/dev/binder` was absent in this
isolated local-Parcel VM). This proves the framework phase ran on Windows; it
does not establish completion of the AIDL/public-manager phase.

The fixture now publishes the production `Services.DisplayManager` Binder and
its existing package Binder into `ServiceManager`'s cache before context
construction. It also creates a real writable `ApplicationSharedMemory` region
for framework cache state. It explicitly checks that a real display and the
framework `ApplicationPackageManager` were constructed, then retains all
original proxy/Parcel, parsed-manifest, public-overload and exact-result checks.
The corrected entry point compiles with the actual Android core/framework and
converts to DEX; its completed Windows result remains a separate CI gate.

That corrected complete ART entry point subsequently **passed on both Windows
2022 and Windows 2025** at published `7e62479c`:
[job 128657747836](https://github.com/gesher-runtime/gesher/actions/runs/42456227216/job/128657747836)
and [job 128657747923](https://github.com/gesher-runtime/gesher/actions/runs/42456227216/job/128657747923).
Both logs report `PASS  services: real activity options, specific intent ordering
and manifest filtering`. The runner requires exit zero and the final
`ActivityOptionsContractTest all passed:` marker, so this establishes completion
of the parsed-APK, generated AIDL/Parcel and actual public-manager checks in that
fixture. The overall focused jobs still failed on other contracts; this result
does not certify a complete Windows test run or all resolver behavior.

## Material remaining limits

This does not establish complete Android package resolution parity. In particular:

- Android returns a real system resolver activity when several equally preferred
  implicit-specific targets require user choice. Gesher has no corresponding
  preferred-activity/chooser backend. This implementation selects the first real
  sorted candidate in that case; it does not fabricate a launchable chooser or
  claim that preference-selection behavior was added.
- The existing package model does not implement full Android application
  visibility, instant-app installation/visibility, domain verification, dynamic
  delivery, profile forwarding, or caller/target compatibility-policy enforcement
  from `SaferIntentUtils`. It also lacks Android's original-system-image versus
  updated-system-app priority history and setup-wizard designation. The helper
  uses the verified addon state actually maintained by Gesher.
- Existing ordinary `queryIntentActivities` and `resolveIntent` behavior is not
  changed by this narrowly scoped implementation.

The missing operation now performs real installed-state resolution and returns
the corresponding options. The limits above require their own backend work and
runtime evidence; neither compilation nor these focused tests eliminates them.
