# Foreground-service manifest permission admission — 2026-11-22

Issue: [#56, F06](https://github.com/gesher-runtime/gesher/issues/56).
Base: `638e19b4a6c109ec8881ba90a94d64b1817fc095`.
Implementation author: `palmoni5`.

## Scope verdict and Android contract

This is a bounded correction to promotion of an installed ordinary application's
service through `Service.startForeground` / `IActivityManager.setServiceForeground`.
It implements the base and type-specific **manifest permission layer** and type
validation under Android 16's default target-SDK compatibility settings. It does
not complete F06 or its separate type runtime prerequisites.

Scope verdict: the change preserves the application's requested operation and
checks its installed owner's permissions before publishing the foreground state
or notification. It supplies no synthetic permissions, capabilities, input, or
package-specific production exceptions. Existing while-in-use capability
calculation, service identity authentication, notification-channel locking and
package-lifetime checks remain on the production path.

The contract reference is AOSP `frameworks/base` tag `android-16.0.0_r4`, matching
`aosp.lock`. The original files were retrieved from official Gitiles with
`?format=TEXT` and decoded without modifications:

| Reference | Git blob | Used behavior |
| --- | --- | --- |
| [ActiveServices.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/am/ActiveServices.java) | `5016e9d62c6f095a7a5ba665401ef9cf2efe999c` | Permission-before-type ordering; manifest subset; mixed short-service normalization; exception identity; unknown-type fallback |
| [ForegroundServiceTypePolicy.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/ForegroundServiceTypePolicy.java) | `355f331afd2bcc216d6ea79eb36dc7b771ef4d1e` | Target 34 compatibility changes; normal type permissions; registered default policies |
| [Service.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/app/Service.java) | `9f09e1d8713e994f5b91f65dc08c9aaf9f65db97` | Public promotion overloads and MANIFEST/NONE distinction |
| [ServiceInfo.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/ServiceInfo.java) | `184b16e37cb2d064a7fab50291696381f85b62a2` | Type constants and public type meaning |
| [PlatformCompat.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/compat/PlatformCompat.java) | `8ed1b537bf7ef25a41d16f8173ad6641429efeb2` | UID compatibility changes require every installed UID member to meet its target threshold |

Public documentation: [launching a foreground service](https://developer.android.com/develop/background-work/services/fgs/launch)
and [Android 14 foreground-service types](https://developer.android.com/about/versions/14/changes/fgs-types-required).

## Confirmed gap and implemented subset

Before this change, promotion checked the notification and manifest type subset,
but did not require `FOREGROUND_SERVICE`, reject target-34 missing/none types, or
check any type-specific normal permission. An unprivileged app could publish
foreground-service state and a notification despite these missing prerequisites.

| Case | Result after this change |
| --- | --- |
| Target below 28 | No newly introduced base permission requirement |
| Target 28+ | The service owner's authenticated UID must hold `FOREGROUND_SERVICE` |
| Requested types not in service declaration | `IllegalArgumentException`, including older targets |
| Target below 34 | Missing/NONE type and missing type-specific normal permissions retain legacy behavior |
| Target 34+, MANIFEST resolves to NONE | `MissingForegroundServiceTypeException` |
| Target 34+, explicit NONE | `InvalidForegroundServiceTypeException` |
| Target 34+, registered ordinary type | Its `FOREGROUND_SERVICE_*` normal permission is required |
| Several selected types | Every selected type's normal permission is checked before mutation |
| SHORT_SERVICE alone | Base permission is required; no invented type-specific permission |
| SHORT_SERVICE combined with other types | The short-service bit is stripped before storing/querying the selected type |
| Unknown type without SPECIAL_USE, target 34+ | `InvalidForegroundServiceTypeException` |
| Unknown type with declared/selected SPECIAL_USE | AOSP's special-use fallback applies; SPECIAL_USE permission is still required |
| Hidden FILE_MANAGEMENT | It is unregistered in the pinned default policy, so it follows the unknown-type rule |
| `stopForeground` / notification ID zero | No promotion permission/type checks; existing foreground state and start eligibility are cleared |
| Failed promotion/update | Existing ID, type, capabilities and notification publication remain unchanged |

The target-34 type-policy compatibility changes are evaluated **per UID**: all
installed members must target 34 or later. An older shared-UID sibling preserves
legacy type behavior even for a newer service owner. The current package snapshot
is used on every promotion, so replacing or removing that sibling updates the
gate. The base target-28 permission requirement remains per owning application,
as in AOSP; permission grants themselves use the UID union.

The manifest permission check uses the installed service UID, including the
existing shared-UID permission policy. A server/system caller cannot lend its
own privileges to an ordinary service. A removed or replaced package incarnation
cannot acquire a new promotion through an old service token.

The AOSP base permission check happens at promotion. This change deliberately
does not add an earlier rejection to `startForegroundService`: launching a
service and promoting it are different operations.

## Regression fixtures and existing notification fixture repair

`ForegroundServiceAdmissionTest` exercises production `ActivityManagerService`,
`ActiveServices`, `PermissionPolicy`, package resolution and notification posting.
It creates explicit test APK metadata and a registered app process; its service
is started through the real manager rather than directly inserting its record.
Notification display uses the existing server-local test sink. There is no
production branch that recognizes the fixture package.

The Linux runner compiles the complete actual `ActiveServices.java` and uses the
existing host initialization/identity JNI boundary. The host has no working
AssetManager, so only framework permission **definitions** are supplied to the
catalog; grant decisions and requested manifest permissions use production code.
The Windows/ART runner uses the real framework assets and generated
IActivityManager Parcel transactions through `ServiceFixtureRunner`.

The older `NotificationChannelGroupsTest` promoted an arbitrary declared service
with type NONE. Its `devsvc.apk` fixture now declares a dedicated special-use
service, the two required normal permissions and a test-purpose subtype property.
The test selects that exact service and uses MANIFEST. It no longer relies on
the missing production permission/type checks to exercise notification lifetime.

## Validation

Local implementation checks, before independent review:

| Gate | Observed result |
| --- | --- |
| Complete current app Java compilation and DEX build | Passed |
| Complete services-test DEX compilation | Passed |
| Updated `devsvc.apk` resource, Java, DEX and alignment build | Passed |
| Foreground admission host fixture | 173 checks passed |
| Original-base negative control | Fails at target-28 missing base permission being accepted |
| Separate original-base type/permission/normalization controls | Fail respectively at missing manifest type, missing data-sync permission and unstripped SHORT_SERVICE |
| Pre-review candidate shared-UID control | Fails because a target-34 owner with a target-33 sibling incorrectly rejects NONE |
| Existing AppOps production host contracts | 482 checks passed |
| Existing service rebinding production host contracts | 431 checks passed |

The host runner accepts `--source-ref` and individual `--scenario` values
(`base`, `types`, `permissions`, `normalization`, `shared-uid`) to establish pre-fix regression
sensitivity. It does not replace production methods with a reimplementation.

CI now runs the host fixture in `.github/workflows/framework.yml` and the same
fixture with generated Binder parcels in `tests/windows/tracks/94-framework-fixes.ps1`.
The pre-existing components foreground-service application sequence and repaired
notification-channel group test remain integration gates. Candidate Windows/ART
results must be reported from the exact PR CI run; no such execution is claimed
by the local checks above.

Local compilation reused source-bound framework/core compile inputs from main
`8e9d442ac7617d82d53f980e58806b107ce26e0f`, workflow run `44715261143`, artifact
`14620400484`. These are historical **compile inputs**, not evidence that the
candidate Windows runtime executed. The minimal local AOSP source tree also
produces only the local-source compatibility table entries, so these local
outputs are not release artifacts. The full CI build uses its own pinned AOSP
sources and complete generated compatibility table.

Independent review of the first candidate identified the distinction between
owner-target base-permission enforcement and UID-wide type compatibility.
The correction adds installed shared-UID metadata, real PM replacement/removal
publication, normal-permission union checks, and an unrelated-UID control.
The new shared-UID scenario reproduces the first candidate's rejection before
passing with the corrected production path. Independent review passed local
commit `3b5cbb28c0f195f69ec3ec41d97dcd863ed87419`, including a separate execution
of all 173 checks. The published commit
`71ccbb7cfe8d67d0e1f0056d00bf250367d44d85` has the identical reviewed tree
`911b089fa98aa1ba63b5d580f95323a6bfe95bb1`.

### Required framework workflow pin

The first PR run, `45020293584`, exposed a wiring gap: `ci.yml` called the
immutable framework recipe at `2c096437d9474a59e4fd6f78d1670c9c7afc573f`, so
declaring the new host step in the candidate's `framework.yml` did not execute
that step. The caller now pins the already-published, reviewed recipe at
`71ccbb7cfe8d67d0e1f0056d00bf250367d44d85`. The complete recipe delta retains
all previous steps and also activates the installer-attribution, Bluetooth-request
and Windows Wi-Fi contract gates already present on the captured main branch.

The replacement retains `local-runner: false`, immutable runner/cache action
pins, existing permissions and main-only cache saves. It changes no runner-group
allowlist or local-runner authorization. The next exact-head run must show the
foreground-admission host step actually executing, as well as the Windows/ART
fixture and notification integration gates; a green run that omits the step is
not evidence for this regression.

### Integration after main's native/runtime update

Main advanced to `1a3a5c244d643c7c93b621326857c6b8c858d5bb` while the PR was
under validation. Integrating that main commit preserves all of its native,
compiler, cancellation-receipt, CI and source-security changes. The only merge
conflict was the immutable framework recipe pin. Main's recipe at `8b316aeb`
and its checked-in `framework.yml` have the same blob
`b84c63efb79aea8f97f484c9ac206921ac3c4627`; the retained reviewed `71ccbb7`
recipe is exactly that file plus this round's foreground-admission step.

Post-integration local verification rebuilt the complete application Java/DEX
output and reran the production host fixture: 173 checks passed. CI YAML parsing,
the six cache-policy tests and the diff check against current main passed.
Structural comparison retains every other upstream-changed path byte-for-byte
and leaves this round's production and fixture source unchanged.

Before integration, exact head `b91a67213eb77fa0636ffad6a636441898b7a7f8`
completed CI run `45020605907`: 74 successful jobs, six expected skips and the
successful `ci-required` aggregate. The Linux 173-check gate and both Windows
foreground-admission/notification fixtures actually passed. Those results
qualify that prior source combination; the integrated head requires fresh CI.
After another parallel permission PR merges, its additional framework gates
must also be retained in a reviewed immutable recipe.

## Remaining F06 work and qualification limits

- Type-specific runtime grants, AppOps, current while-in-use eligibility,
  role/default-dialer predicates, USB grants, projection authorization and system
  exemption alternatives need their own source-supported implementation rounds.
  A successful normal-permission check does not establish these prerequisites.
- Background foreground-service start restrictions and their actual exemptions,
  launch-to-promotion deadlines, ANR/error paths, short-service timeouts and
  long-running data/media limits remain incomplete.
- Instant-app policy and developer compatibility/DeviceConfig overrides are not
  newly implemented here; ordinary installed applications and Android's default
  target behavior are the bounded profile.
- AOSP source inspection establishes the oracle used, but no real Android device
  differential run, complete third-party APK corpus, or Windows physical-device
  test was performed locally. F06 stays open.
