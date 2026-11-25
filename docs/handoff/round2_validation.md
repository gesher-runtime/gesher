# Round 2 continuation status and validation

## Current continuation — 25 November 2026

Account: `palmoni5`. The continuation remains in existing draft
[PR391](https://github.com/gesher-runtime/gesher/pull/391).
The branch now incorporates main
`a0291539acd9fef036a305c23a0cd33dd297048a` and the successful runtime diagnostic
`908d99eb480b6d40480235e13c678fb791e7e636`.

- PR381 and PR399 have already merged. Their R1, graphics and usage production
  changes are retained exactly from main; PR388 is externally closed, not
  merged. The continuation does not reopen it or repeat those implementations.
- The nine remaining production differences from the pinned main are WebView
  files. Their contents still match the previously published `4fe77c03`
  continuation; worker entry points remain unavailable. Main's later WebView
  provider allocation improvement is retained.
- The [fresh open-PR audit](../research/webview-open-prs-2026-11-25.json) fetched
  all 86 listed pull refs and inspected their file diffs. PR493 advanced between
  listing and fetch; both identities are recorded and its fetched code was
  inspected. PR414 is the only other open PR intersecting the remaining
  production files: its change converts profile paths in `Host::Configure` and
  is independent of the storage/worker continuation. No other PR was changed.
- The corrected quota experiment passed its complete persisted/live/deletion/
  payload sequence on WebView2 131 and 157 in actual Windows run 45498551083.
  The [runtime report](../research/webview-runtime-results-2026-11-25.md) and
  linked machine-readable evidence retain exact source, artifact and job IDs.
  This proves the measured registry mechanism; it does not complete a production
  `getOrigins` implementation or arbitrary per-storage-key effective quotas.
- The same runtime experiment showed that all three injected worker stream
  faults produced successfully consumed HTTP 200 bodies. It does not qualify
  Android network-error behavior, cache modes or progressive streaming.

The reconstruction preserves main's complete selected-source qualification
policy and every workflow file. Only the four successful quota-probe repair/
test files are imported from the diagnostic branch; its temporary push trigger
is excluded. Independent inspection found no reconstruction defects. The
reconciled actual player suite passed 240 checks with ASan, UBSan and
float-cast-overflow instrumentation, including main's large-timestamp regression
and the continuation's feeder-room/starvation cases. LeakSanitizer alone was
disabled because the local process is traced.
The unchanged current-main selected-source policy also passes on this combined
tree (`TCP_SELECTED_POLICY_PASS`); it starts no native processes and does not
replace the Windows acceptance gates.

These checks and the historical runs below do not substitute for full CI on the
newly published integration. Full WebStorage and worker completion remain open;
the current continuation is not ready for final approval.

## Historical validation record — 23–24 November 2026

The following records describe their stated commits and runs. References to an
active PR388 or unmerged R1/graphics/usage work reflect that earlier state and
are superseded by the current continuation above.

Date: 2026-11-23. Account: `palmoni5`.
PR: [#391](https://github.com/gesher-runtime/gesher/pull/391).
Round 1: [#388](https://github.com/gesher-runtime/gesher/pull/388).
Base: `bcfb5044029d486e17138dc417f3ced81524cf3f`.

The final continuation also merges main `3b2eaca95b5ddd9cee3c3db4e430460e512eb355`.
The earlier CI checkout combined the PR with that newer shortcut-accounting
change, exposing missing process/activity state in the usage-hook fixture.
Validation of the correction uses the actual combined source, not a shortened
copy of the production hook.

The continuation is now cumulative: reviewed R1 head `b7b128d2` is a genuine
merge parent, and every R1 change is retained. The integration tree is
`2cc49d5d5f922e10e0d9f7a826ab37f879a0211d` (reviewed local merge `9ff31dfb`,
published merge object `388b6c43`). PR #388 remains unchanged and independently
reviewable; PR #391 carries the combined source and the R2 follow-ups. Preserving
R1's ancestry when it lands retains this relationship. A squash/rewrite would
instead require an explicit branch reconciliation.

## Scope status

| Handoff item | Status |
| --- | --- |
| Surface damage, retained window/layered copies, dynamic refresh | Implemented, host-validated and independently reviewed |
| One-shot application usage observers | Implemented; independent deadlock and callback-cancellation findings repaired |
| Real per-origin storage measurements | Implemented, with native URL admission and actual Chromium usage/quota fields |
| Framework/AndroidX worker request bridge | Inert preparation; all worker feature flags withheld, controller getters explicitly unavailable and native activation rejected before scheduling/filter installation |
| Worker cache modes, complete file/content parity, complete storage-origin enumeration/partition deletion | Explicitly incomplete; unsupported feature flags are withheld |

This is not completion of the full #56/#178 roadmaps. PR #391 remains a draft
because W07/W08 are not complete. In particular, a synthetic HTTP 403 is not an
Android network error. The entire worker activation path is now unavailable;
feature queries cannot enable its filter, and direct framework/AndroidX/native
entry points cannot activate the prepared bridge. This removes unsupported
advertising and synthetic worker responses from the new production path without
claiming W07 completion. The worker/storage document records the independently
verified native-API and CDP limitations and the required Windows acceptance gate.

## Implementation review

The integrated implementation was reviewed at local `fa114ebc`, equivalent to
remote `7d2edbca`. Original scoped commits are preserved as separate changes.
The following integration commit adds test-entry wiring and validation evidence.

- Graphics: retained damage and full-copy fallback, buffer generations, queued
  unions and duplicate presentation messages were inspected and regression-tested.
- Usage: a separate reviewer reproduced PM/ATM deadlocks in all four admission
  paths and revocation of already-ready callbacks. Both defects were fixed and
  the original reproductions now pass; old-source negative controls still fail.
- WebView: root inspected the actual Java/JNI/native diff. A separate native API
  review confirmed the failure-contract blocker. A subsequent scope review found
  that capability discovery itself enabled the incomplete worker filter, and
  that withholding only its provider flags would leave framework access active.
  The Java/common getter and native activation are now explicitly unavailable;
  capability discovery has no initialization side effect. Storage/cookie helpers
  and the reserved JavaScript-bridge guard are unchanged. W07 remains blocked.

## Local results

| Validation | Result |
| --- | --- |
| API 37 report suite on combined source | 37 selected/executed; 0 failures, errors or skips |
| Complete unchanged app build with actual framework/core classes | All Java and AIDL delegates compile; BootLinkCheck 292 references / 0 unresolved; D8 passes |
| Complete activity-finish CI entry on the final main merge | 901,542 checks pass with fresh app classes and actual framework/core inputs |
| Query model in that combined run | 12,661 checks pass |
| Usage observer helper | 1,117 checks pass; full service boundary and actual ATM callsite tests pass |
| Surface geometry / release / damage | 158 / 38 / 75 checks pass |
| Layered / presentation allocations | 1,441 / 38,028 checks pass |
| Frame recycling / video scaling | 331,204 / 32,165 checks pass |
| Readback / display refresh / mouse | 12,000 / 44 / 25 checks pass |
| Java worker availability and independent storage | 206 feature-query checks and 310 fresh public-entry/storage checks pass; two activation mutations rejected |
| Prepared worker/settings/query Java components | 63 isolated component checks pass; the fixture alone publishes its prepared controller and does not establish production availability |
| Native worker availability and independent storage | 101 checks pass with ASan/UBSan, zero worker filters installed; exact historical activation control rejected after three recorded filter installations |
| Prepared native worker ownership / timeout / completion | 314 isolated checks pass with ASan/UBSan; missing-denial negative control rejected; this does not establish faithful network errors |
| Existing WebStorage deletion / boundary / owned streams | 16 / 32 / 17,352,558 checks pass |
| Production native compilation | Relevant graphics/window and complete WebView host units compile for Windows x64 and ARM64; complete patched-JNI builtin/shared units pass on x64. ARM64 patched JNI has the pre-existing `sysv_abi` limitation detailed below |

Existing CI entry points now invoke the new observer, refresh and worker suites;
no workflow files are modified. Compiler/sanitizer settings propagate to the
native worker suite, including its negative control.

The final gate tests were independently reviewed at local `a44350ef` (published
as `d5e4ae9f`), above production repair `69ef51d4` (published as `392eb50f`).
Fresh JVMs exercise the real provider methods and
whole controller/storage-query classes before any prepared component fixture
is constructed. A separate capability-query fixture makes its recording worker
callee usable, so a downstream exception cannot conceal an initialization
attempt. The native control restores the exact published `fed039fc` activation
and service-controller functions, including scheduling, COM capability query,
filter installation and completion. The rejected control installs three worker
filters; the current entry installs none. Storage and cookie helper bodies are
unchanged. These results demonstrate unavailability without claiming W07 support.

The main-merge usage fixture repair reproduced all 15 original compilation
errors, then retained the complete production `recordUsage` body and extracted
`activityCodeCurrent` predicate. Recording process/activity/package boundaries
cover valid, retired, replaced and removal-pending lifecycle state without
suppressing the usage transition. The exact CI activity-finish command and all
its groups pass against freshly built combined app classes. The unchanged app
build also passes again: BootLinkCheck remains 292 references / zero unresolved,
and D8 completes.

Independent review of the fixture also caught reused recording state that could
hide a skipped transition in a later case. Each case now uses a fresh sink; all
five targeted transition-suppression mutations fail their corresponding delivery
assertion. The final focused observer/service/hook suite passes. Both reviewed
fixture corrections are published together as `7e8d3014`; production behavior
and the complete extracted methods are unchanged.

Graphics ABBA measured CPU copying over 400 1080p frames. The 64x64 dirty path
took 2.408 / 2.368 ms in the two candidate passes versus 980.107 / 886.432 ms for
the baseline full-copy path. Checksums matched. Full-frame timing was noisy and
does not establish a speedup. Usage's workload schedules one deadline for 1,000
observers and 10,000 duplicate lifecycle events, with no inactive polling.
No performance comparison against a no-op worker/storage/observer baseline is
claimed.

### Public WebStorage runtime acceptance

The existing windowing track (`37-webview.ps1`) now launches
`WebStorageActivity` from `webapp.apk` in a fresh application profile. Its normal
`WebViewClient` supplies the HTML for two fresh, distinct HTTPS origins; this is
a page-response fixture, not a network-connectivity claim. Each origin commits
and closes a real 256 KiB IndexedDB record. Public `WebStorage` usage/quota calls
must report positive measurements for both origins, including A while B is the
visible page. After `deleteOrigin(A)`, bounded polling must observe zero usage
and positive quota before reopening A. An aborted database upgrade then proves
A's database is absent, while B's complete sentinel and positive measurements
must remain. Public callbacks are checked for unique delivery and the invoking
UI thread. Per-call, convergence, page and total deadlines produce explicit
failures; the existing process runner supplies the outer bound and cleanup.

The unchanged APK build passes with actual Android framework/core classes,
`aapt2`, D8 and `zipalign`. JavaScript and PowerShell syntax checks pass. Isolated
PowerShell transcript controls accept a complete result and reject five cases:
zero baseline usage, missing database-absence proof, lost unrelated data, a
callback failure after the success marker, and a nonzero process exit. These
are harness checks only.

Real Windows/ART acceptance subsequently passed on published head `e909052a`
in [CI run 45230894563](https://github.com/gesher-runtime/gesher/actions/runs/45230894563).
The checkout was merge `35b0626158563419fef5ea84625a12e5c3951a1c` with main
`e505ad22`. These observations belong to that exact run; later heads still need
their own complete CI result.

| Runtime observation | Windows 2022 | Windows 2025 |
| --- | ---: | ---: |
| WebView2 version | 131.0.2903.86 | 157.0.4341.46 |
| Committed payload per origin, bytes | 262,144 | 262,144 |
| A baseline usage, bytes | 109,154 | 107,767 |
| B baseline usage, bytes | 109,830 | 107,855 |
| Reported quota per origin, bytes | 96,635,503,410 | 96,635,503,410 |
| A usage after deletion | 0 | 0 |
| A database after reopening | Absent | Absent |
| B complete payload after deleting A | Preserved | Preserved |
| Public queries / callbacks | 10 / 10, main thread | 10 / 10, main thread |

Both executions first observed A's old usage after deletion, then converged to
zero within the deadline. Database absence and preservation of every byte in B
were verified independently afterward; B's final usage remained unchanged.
Both complete WebView/windowing tracks reported zero failures.
[Windows 2022 log](https://github.com/gesher-runtime/gesher/actions/runs/45230894563/job/137899912777)
and [Windows 2025 log](https://github.com/gesher-runtime/gesher/actions/runs/45230894563/job/137899912709)
were independently reviewed and then checked by the integrating reviewer.
This closes the real-runtime acceptance gap for these per-origin operations;
complete origin enumeration and partition-aware deletion remain open.

The same jobs executed the bounded browser-session diagnostic with SDK
`1.0.4258.31` and the static loader. Setup, `_11`, subscriptions, discovery and
cleanup all returned success, but browser-filtered enumeration returned
`{"targetInfos":[]}` and no browser target event was observed. Neither job
attempted attachment or the session browser-scope command without a target ID.
The result is unavailable through these tested routes on these runtimes; no
Fetch coverage or worker capability was established. This is not a claim that
every possible browser-session route is impossible.

### Trusted-local unmanaged usage fixture

Both services jobs on `e909052a` reached the same outdated fixture assertion.
`SoftwareServiceContractTest` calls the service directly in its own process,
which real `CallerIdentity` treats as trusted local control-plane authority.
With no managed activity lifecycle, that authorized call correctly reports
`UnsupportedOperationException`; it is not an unprivileged caller's permission
denial. Production already authenticates before checking this capability.

The fixture now verifies the actual local Binder PID and `isSystem()` result,
uses a valid 60-second limit and non-null `PendingIntent`, and requires prompt
unavailability. Its original `SecurityException`-only denial helper and
session/limit checks remain. The exact-source host suite separately verifies
that missing-permission and foreign-package calls are denied before the
unmanaged gate. A control reversing that order fails the new assertion.
The focused service suite and the updated runtime fixture's compilation/D8
against real Android framework/core classes pass. No production permission
check was changed.

After the cumulative merge, the complete unchanged `app/build.sh` also passes
against the established actual framework/core inputs: generated AIDL delegates,
all Java sources, BootLinkCheck's 292 references with zero unresolved members,
and D8. Source hashes were checked before and after the build. This is combined
compile/link/DEX evidence; the current Windows/ART result belongs to the PR's
exact-commit CI checks.

### Cumulative media feeder causal regression

The Windows 2022 media job `137899912583` in run `45230894563` captured a real
mid-tone dropout on R2 `e909052a`: 1,967 stereo frames of digital silence at
48 kHz, from 10.347750 to 10.388729 seconds. The AAC tone's local pitch on both
sides remains approximately 587 Hz; the unchanged detector reports 559 Hz
because its interval includes the gap. The exact CI merge `35b06261` has the
same media sources as that R2 head. This is not a reason to relax the detector
or its tolerances.

The cumulative merge `388b6c43` (reviewed locally as `9ff31dfb`) carries R1
`b7b128d2`, including its
room-notified `WritePcm`, seek flush and pre-join Track release. This follow-up
adds only a causal regression and its documentation: the complete production
`player.cc` remains byte-for-byte R1. The full R1 tests, clock/wait observations,
timeout override and measurement mode are retained. The new cases share those
existing boundaries and compile the real clocked sink's backlog loop with an
explicit clock. The old timer is held by a predicate, so spurious notifications
cannot bypass the controlled delay.

The combined player entry passes 235 checks under ASan/UBSan. With timer expiry
withheld, all three sink pieces refill, no underrun or interior silent run
occurs, and seek/shutdown wake the feeder without its safety timeout. A separate
scratch-generated control restores only the exact `e909052a` `WritePcm` body;
the rest of R1 remains intact. It fails exactly seven expected assertions: the
existing stalled-feeder wake-count check and six new refill/silence checks.
It refills zero of three pieces, with one underrun and 1,981 interior zero
frames. A held-decoder case retains one underrun and 1,737 interior zero frames
with both versions. The unchanged captured-tone analyzer and tolerances pass
in both runs. No production source, buffer size, capture timing or media
acceptance threshold is changed by this regression addition.

This demonstrates the missed-room-notification defect addressed by R1. The
uninstrumented Windows recording cannot distinguish that mechanism from a
decoder stall, which the correction does not eliminate. The pre-publication
host result does not establish that the observed Windows dropout is fully
resolved; cumulative Windows acceptance is recorded in the PR's exact-commit
CI checks. LeakSanitizer is disabled
locally for the tracing limitation described below; ASan/UBSan stay enabled.

### Cumulative Windows results and camera collection readiness

Published head `21e0e02e` was exercised by
[run 45242201319, attempt 2](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/attempts/2),
using exact checkout `5d5cc903ecbecc9391573eea3faa9c4140061987` with main
`08146b1e`. Attempt 1 had stopped the affected Windows jobs at artifact
admission because the shared installation quota was exhausted. After its
logged reset, one failed-jobs-only retry retained the successful producers and
their original evidence; authentication and source-identity checks stayed intact.
Attempt 2 completed with 70 successful jobs, six conditional skips and four
failures across the full 80-job inventory. The one direct camera failure below
caused both Windows aggregates and `ci-required` to fail; all four auxiliary
workflows passed. This is not a fully green result.

Both actual services shards passed the corrected trusted-local fixture under
ART, requiring process exit zero and its final all-passed marker. Successful
fixture stdout is suppressed by the existing runner; its enclosing contract
check and both complete services shards report success.
[Windows 2022](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/job/137963990699)
and [Windows 2025](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/job/137963990690)
were independently reviewed, then checked by the integrating reviewer.

Both windowing shards also passed the real per-origin WebStorage acceptance:

| Cumulative runtime observation | Windows 2022 | Windows 2025 |
| --- | ---: | ---: |
| A baseline usage, bytes | 109,396 | 107,365 |
| B baseline and preserved usage, bytes | 109,380 | 107,387 |
| Reported quota per origin, bytes | 96,635,503,410 | 96,635,503,410 |
| A after deletion | Usage 0; database absent | Usage 0; database absent |
| B complete payload after deleting A | 262,144 bytes preserved | 262,144 bytes preserved |
| Public queries / callbacks | 10 / 10, main thread | 10 / 10, main thread |

The runtime versions remained `131.0.2903.86` / `157.0.4341.46`. Both observed
the old usage before deletion converged to zero, then independently checked
database absence and the entire unrelated payload. The browser-session probe
again returned no browser target and established no Fetch or worker capability.
[Windows 2022](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/job/137963990626)
and [Windows 2025](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/job/137963990878)
each finished the complete windowing track with zero failures. W07 and full W08
remain incomplete.

All captured engine/app audio tones passed on both systems, including AAC tone
11 at 587.5 Hz (0.810 / 0.815 seconds). This is actual acceptance of the
cumulative audio source, while the causal limits above still apply.
[Windows 2022 media](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/job/137963990651)
passed all 299 checks. [Windows 2025 media](https://github.com/gesher-runtime/gesher/actions/runs/45242201319/job/137963990858)
passed 298 checks and failed one single-process Camera2 recording check. The
same Windows 2025 device-mode camera recording passed, as did both Windows
2022 camera modes.

The failing camera open took 16,355 ms. `endConfigure` waits at most 10,000 ms;
the old fixture then started its fixed two-second collection window immediately
after submitting a repeating request. Collection ran from 773.366095300 to
775.366469000 seconds; the capture loop later logged its end at 779.719474500
seconds after the 16,355 ms open. That loop-end value is not a separately
instrumented native-open completion time. No frame had begun or completed.
The unchanged checks correctly rejected the
resulting 0.0-second, zero-frame file. Camera, recorder, muxer, fixture and
PowerShell harness sources are byte-identical between `21e0e02e` and the actual
CI checkout. The open trace spans the whole native initialization; it does not
identify a particular Media Foundation substage or establish an improvement in
startup latency.

The fixture correction `c1624bc8` (reviewed locally as `aefb7cd4`, identical
tree `7d5c84b0`) preserves recorder-start/repeating-request ordering,
then awaits a real
[`onCaptureStarted`](https://developer.android.com/reference/android/hardware/camera2/CameraCaptureSession.CaptureCallback#onCaptureStarted(android.hardware.camera2.CameraCaptureSession,android.hardware.camera2.CaptureRequest,long,long))
callback for at most ten seconds before the unchanged two-second collection.
It reports an explicit readiness failure if capture never starts. Session,
recorder, camera and callback-thread cleanup is still attempted; a secondary
empty-recording `stop()` error is retained beneath the original failure.
Decoding is not attempted after a readiness failure. The existing codec,
frame-count, duration, picture, ordering, close and process-exit gates are
unchanged. No production camera/recorder code or workflow is changed.

The complete fixture method, callback bodies and cleanup run in the new host
regression through the existing Camera2 CI entry. Declared camera/API, clock
and codec boundaries let it exercise a capture delayed by 6.355 seconds and a
capture that never arrives; latches and callback-thread execution remain real.
The existing entry passes 1,036 capture-dispatch checks plus 30 readiness/cleanup
checks. The exact historical `21e0e02e` method fails 18 expected assertions.
This host fixture verifies readiness ordering and owner cleanup, not encoded
camera images. The unchanged APK build also passes against real Android
framework/core inputs, including resource processing, D8 and alignment.
Both the integrating and independent reviewers approved the exact four-file
test/tool change; corrected Windows acceptance belongs to its successor's own
CI result.

An independent timestamp check compiled the unchanged production Recorder and
MP4 writer translation units with UBSan and the existing explicit host
input/encoder boundaries. A real 6.355-second wall-clock delay moved the first
accepted PTS from 51 to 6,355,478 microseconds. Both outputs independently
demuxed and decoded as 60 H.264 packets/frames, start time 0.000000 and duration
1.999544 seconds. The single-video-track MP4 normalizes its earliest sample;
awaiting actual capture before the existing collection window does not require
relaxing the duration check. This host check does not execute Windows capture.

## Limits

The build uses actual prebuilt Android framework/core classes. The local AOSP
Java source inputs needed for the full SOURCE-retention compatibility table are
absent, so the resulting app is a validation artifact, not a distribution build.
The host framework fixtures use controlled native identity and acknowledgements; they
do not prove ART, real Binder/Parcel transport, native resource loading or
Windows presentation. Cross-compilation does not execute WebView2, display
drivers or physical hardware. Local LeakSanitizer cannot start under this
container's tracing; ASan and UBSan run with `detect_leaks=0`, and CI sanitizer
settings remain unchanged. Explicit lifetime checks remain active.

The worker-gate review repeated full native syntax checks with the actual CMake
definitions, including `NOGDI`, and a temporary AOSP JNI header patched by the
repository's two `libnativehelper` patches. WebView host checks pass on x64 and
ARM64; builtin/shared JNI checks pass on x64. ARM64 rejects the existing
unconditional `sysv_abi` calling-convention attribute under `-Werror`. The JNI
source is identical to published pre-gate base `fed039fc`. The earlier ARM64 JNI
checks used the original AOSP header, so they do not prove the patched ABI.
This gate does not change that separate ARM64 issue or suppress its warning.
The canonical ARM64 toolchain has no override for it, and the existing ARM64 CI
build script selects native probes/ELF/bionic/POSIX targets rather than WebView
and JNI targets; this diagnostic is not a regression in those selected targets.

Latest CI status belongs to the PR checks, not this local validation record.

## Detailed records

- [Graphics](../audits/round2-graphics-validation.md)
- [Usage observers](../research/usage-observer-contracts.md)
- [Worker/storage contracts and remaining work](../research/service-worker-storage-contracts.md)
