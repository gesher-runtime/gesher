# Microphone runtime-permission admission

## Scope verdict and roadmap boundary

This implements the live `RECORD_AUDIO` permission layer used by the existing
AudioRecord, MediaRecorder, AAudio and OpenSL ES admission paths. It contributes
to issue #56's F09 permission enforcement and the M10/M16/M17 recording paths.
It does not complete any of those roadmap entries.

The scope is compliant: ordinary callers must have their real UID's current
runtime grant before an existing admission point can accept microphone input.
Requested manifest metadata, a foreign package name, an incidental system Binder
caller or an unavailable permission resolver cannot substitute for that grant.
The change does not supply substitute audio, advertise a capability or fabricate
a successful hardware operation. Existing capture backend and error paths remain.

The source baseline is `638e19b4a6c109ec8881ba90a94d64b1817fc095`. A live review
of the open PRs found no other change to `AudioService.recordAudioAllowed` or its
JNI resolver. PR #107's AAudio/OpenSL changes concern library export registration;
PR #249 changes permission performance internals while retaining the permission
query API. Their larger scopes are not claimed by this change.

## Android contract evidence

Sources were read from the official Gitiles repositories at the runtime's pinned
`android-16.0.0_r4` tag. The blob IDs identify the exact retrieved bytes.

| Source | Exact blob | Relevant contract |
| --- | --- | --- |
| [ServiceUtilities.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/utils/ServiceUtilities.cpp) | `8966544108a0688103b51666cfc3e126dd98c5d3` | Resolve missing native/OpenSL package metadata through the actual UID; runtime grants must not be cached. Initial recording admission rejects hard denial, while soft AppOps denial is a separate result. |
| [AudioPolicyInterfaceImpl.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/services/audiopolicy/service/AudioPolicyInterfaceImpl.cpp) | `821a8a585d0b7f1e6d9ed063bc182fd1985d83c8` | Input start independently rechecks permission and distinguishes hard rejection from a silenced soft denial. That later lifecycle is not implemented by this initial-admission change. |
| [MediaRecorderClient.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libmediaplayerservice/MediaRecorderClient.cpp) | `4d70a71c57a22f97f6cd25623b9d6f3036712d25` | `setAudioSource` validates source requirements and calls `recordingAllowed` before accepting the source. |
| [AudioRecord.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/java/android/media/AudioRecord.java) | `d28cb3bb9a6d2010fb6fa1c6b3d6140cf78eac94` | A failed native setup leaves the constructor result uninitialized; the builder rejects an unusable record. |
| [AudioRecord.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaudioclient/AudioRecord.cpp) | `af390c1ba4e195117c46529678a561fbfd98d954` | Native record creation carries the caller identity to input admission. |
| [AudioStreamRecord.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaaudio/src/legacy/AudioStreamRecord.cpp) | `171eccd6f017ef2295fde197e9e23f4df7de5220` | AAudio uses the underlying record result and converts initialization failure to its API result. |

The public [AudioRecord contract](https://developer.android.com/reference/android/media/AudioRecord)
requires `RECORD_AUDIO`. The [MediaRecorder guide](https://developer.android.com/media/platform/mediarecorder)
also distinguishes a manifest declaration from the runtime permission required
by modern applications.

## Production behavior

`AudioService.recordAudioAllowed()` now queries the production package manager's
UID permission API using `Process.myUid()` for each admission. This uses the same
runtime decision store and shared-UID union as other permission consumers.
Clearing and finally restoring incidental incoming Binder identity keeps a local
query from inheriting an unrelated caller. Package-manager exceptions and any
result other than `PERMISSION_GRANTED` deny admission.

An ordinary UID without installed permission ownership is denied. An installed,
permitted native caller can still record when ActivityThread has no package
metadata. Real system/control-plane UID behavior remains the existing central
permission policy; a claimed package name or incoming system Binder identity
cannot make an ordinary process into that UID.

The existing `MayRecordAudio` JNI bridge now denies if its Java class or method
cannot be resolved. It clears lookup exceptions and releases acquired references.
An absent Java environment, a throwing Java policy or a denied Java result also
denies. The NDK recording provider continues to call this live bridge; an absent
provider already denies through `RecordingAllowed`.

NDK-first use does not depend on constructing a Java AudioRecord. The OBJECT
library in `cmake/aosp/media.cmake` links `audio_record_jni.cc` into dalvikvm,
including its static provider registration. `Main.bootstrap` (also called by
`GesherAppProcess`) loads `android_runtime`; its `gRegJNI` media entry calls
`register_gesher_media`, which sets the Java VM before registering AudioRecord.
The OpenSL test activity loads its NDK test library directly in `onCreate`.
Independent review verified this source ordering. The host fixture explicitly
registers its JNI/provider boundary and does not itself execute ART startup;
the existing Windows OpenSL application cases cover that integrated first use.

Existing callers and their error translations are retained:

| Existing admission point | Retained denied behavior |
| --- | --- |
| AudioRecord native setup | Native initialization failure; public constructor is uninitialized, `startRecording` rejects, builder throws `UnsupportedOperationException`. |
| MediaRecorder audio-source selection | Existing `RuntimeException`/recorder error path. |
| AAudio input open | Existing unavailable result before capture allocation. |
| OpenSL recorder creation | Existing permission-denied result before object allocation. |
| OpenSL deferred realization | Existing repeated provider check and unrealized failure path. |

This is a live query at those admission points. It does not add a new query to
every start/read, track an active AppOps session, silence a running stream or
claim to stop an already-open recorder on revocation. AOSP's later hard/soft
start distinction remains a separate implementation requirement.

## Fixtures and validation

`AudioRecordingPermissionTest` constructs explicit installed metadata and uses
the real `PackageManagerService`, `PermissionPolicy` and complete `AudioService`
class. It covers manifest-only denial, grant/revoke transitions, foreign package
and incoming Binder identities, absent package metadata, the shared-UID union,
real package replacement/removal publication, an unknown UID and resolver errors.
Faulting package-manager adapters are confined to resolver-failure tests; grant
decisions come from the production permission store.

The host fixture compiles the exact `MayRecordAudio` body and actual
`record_permission.cc`, crosses JNI into that real Java policy, and checks both
the direct bridge and the provider used by native input adapters. Separate native
controls inject JNI class/method/exception outcomes without replacing the bridge
body. Host framework initialization, process/Binder identity, cache infrastructure
and permission **definitions** are explicit fixture boundaries. The Windows
fixture instead uses framework assets, generated package-manager Binder parcels
and real public AudioRecord/MediaRecorder native calls for denied admission.

| Check before independent review | Observed result |
| --- | --- |
| Production Java/JNI/provider fixture, using freshly built app classes | 50 checks passed |
| Native resolver/failure/provider fixture | 30 checks passed |
| Original main Java control | Fails because a modern manifest declaration without a runtime grant was accepted |
| Original main native control | Fails because a missing Java permission class was accepted |
| Existing AAudio builder/admission contracts | 105 checks passed, including denied input and cleared output |
| Complete app Java and DEX build | Passed |
| Complete services-test DEX build | Passed |
| Changed PowerShell parsing and existing cache policy tests | Passed |
| Windows/ART, actual OpenSL realization and recording integration | Pending exact-head PR CI; no local Windows execution claimed |

The host runner accepts `--source-ref` and separate `--java-only`/`--native-only`
controls. Both original-source failures above execute the original implementation,
not a deliberately weakened copy of the new implementation.

The recordapp and positive OpenSL APK cases now receive explicit fixture-operator
grants through the real permission API. They keep their admitted application UID.
Installed-device cases grant again after real installer publication and verify
the live remote service; a prior standalone grant is not assumed to survive a
fresh install. Denied APK and standalone/native cases retain their denial checks.
This changes test setup, not ordinary application grant behavior.

The shared `FixturePermissions.ps1` helper and recordapp track are byte-identical
to the independently reviewed camera round's `6a3e6c30` versions. recordapp uses
both declared CAMERA and RECORD_AUDIO permissions. The Windows OpenSL test adds
its own microphone grant setup with profile/environment restoration. Existing
native deferred-realization revocation coverage stays in the mandatory media
track's `opensl-permission` case.

Local framework/core compile inputs came from historical source-bound main
`8e9d442ac7617d82d53f980e58806b107ce26e0f`, run `44715261143`, artifact
`14620400484`. They are compile inputs, not a candidate Windows runtime result.
The minimal local AOSP source tree emits only local-source compatibility table
entries. These local DEX outputs are not release qualification; exact-head CI
builds the full pinned source and compatibility table.

## Independent review and CI wiring

Independent scope and correctness review passed candidate
`ef7eec7c8d35c594f6e86799f442f925b2521ab6`, tree
`bc810394ea57d85f04662fe1c5efb9cec58c5038`. The reviewer rebuilt the application
Java/DEX classes and independently reran the 50 Java/JNI/provider checks, 30
native failure controls, 105 AAudio checks, both original-source negative
controls, all four changed PowerShell parses and the shard-planner checks.
The review also verified first-use provider registration and the exact shared
camera fixture/helper bytes. Windows/ART execution remains an exact-head CI gate.

The reviewed source was published by `palmoni5` as
`8332eac1a6d0a62f684cbc8fd3fc77825d680d61`; its remote tree exactly matches the
reviewed tree above. The caller now pins `framework.yml` to that immutable
published recipe, retaining `local-runner: false` and existing runner, action,
checkout, cache, permissions and artifact policies. The recipe checks out the
caller revision and tests its freshly built classes. The pin also retains the
installer-attribution, Bluetooth and Wi-Fi host gates already on the baseline
but absent from the previously pinned recipe. Review scanned all changed paths
and baseline blob IDs for static source-integrity receipts; none require a
refresh. The `ci-processes` OpenSL function-body check remains unchanged.

Reviewing the declaration alone does not establish that CI executed it.
The exact-head run must show the microphone host step and Windows public/native
and existing recording gates actually executing.

### Integration after main's native/runtime update

Main advanced to `1a3a5c244d643c7c93b621326857c6b8c858d5bb` while the PR was
under validation. Integrating that main commit preserves all of its native,
compiler, cancellation-receipt, CI and source-security changes. The only merge
conflict was the immutable framework recipe pin. Main's recipe at `8b316aeb`
and its checked-in `framework.yml` have the same blob
`b84c63efb79aea8f97f484c9ac206921ac3c4627`; the retained reviewed `8332eac1`
recipe is exactly that file plus this round's microphone-admission step.

Post-integration local verification rebuilt the complete application Java/DEX
output and reran the production host fixtures: 50 Java/JNI/provider checks and
30 native resolver controls passed. CI YAML parsing, the six cache-policy tests
and the diff check against current main passed. Structural comparison retains
every other upstream-changed path byte-for-byte and leaves this round's
production and fixture source unchanged.

The prior exact head `14c32630a0d67c1d4930f55fe601ca2384381f82` passed the fresh
host-tools receipt, 30 native resolver checks and 50 Java/JNI/provider checks
in run `45023393219`. Its first predecessor run encountered malformed GitHub
jobs-API JSON before compilation; a transparent empty commit triggered a full
normal CI run with fresh coherent suppliers, without changing source or guards.
Those prior-head results do not qualify the integrated runtime; the integrated
head requires fresh CI, including all Windows recording paths.

## Parallel PR integration

The foreground-service, camera and microphone rounds each pin their own reviewed
recipe. After one merges, remaining PRs must refresh their branch and pin to a
reviewed published recipe containing the union of merged and new gates. Choosing
one competing SHA without retaining the others' steps would drop coverage.
No runner-group authorization, action pin, permissions or CI success requirement
is loosened by this work.

## Remaining qualification

Active-session revocation, permission changes between setup and later start,
AppOps attribution/start/finish and soft-denial silencing, privacy controls and
concurrent-recording policy remain outside this change. Real microphone hardware,
a complete Android-device differential and all special/system capture sources
have not been qualified by these host fixtures. F09 and the audio roadmap entries
remain open.
