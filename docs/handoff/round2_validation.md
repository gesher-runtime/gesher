# Round 2 continuation status and validation

Date: 2026-11-23. Account: `palmoni5`.
PR: [#391](https://github.com/gesher-runtime/gesher/pull/391).
Round 1: [#388](https://github.com/gesher-runtime/gesher/pull/388).
Base: `bcfb5044029d486e17138dc417f3ced81524cf3f`.

The final continuation also merges main `3b2eaca95b5ddd9cee3c3db4e430460e512eb355`.
The earlier CI checkout combined the PR with that newer shortcut-accounting
change, exposing missing process/activity state in the usage-hook fixture.
Validation of the correction uses the actual combined source, not a shortened
copy of the production hook.

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

### Cumulative media feeder causal regression

The Windows 2022 media job `137899912583` in run `45230894563` captured a real
mid-tone dropout on R2 `e909052a`: 1,967 stereo frames of digital silence at
48 kHz, from 10.347750 to 10.388729 seconds. The AAC tone's local pitch on both
sides remains approximately 587 Hz; the unchanged detector reports 559 Hz
because its interval includes the gap. The exact CI merge `35b06261` has the
same media sources as that R2 head. This is not a reason to relax the detector
or its tolerances.

The cumulative merge `9ff31dfb` carries reviewed R1 `b7b128d2`, including its
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
decoder stall, which the correction does not eliminate. **Windows acceptance
of the cumulative source remains pending**; the host result does not establish
that the observed Windows dropout is fully resolved. LeakSanitizer is disabled
locally for the tracing limitation described below; ASan/UBSan stay enabled.

## Limits

The build uses actual prebuilt Android framework/core classes. The local AOSP
Java source inputs needed for the full SOURCE-retention compatibility table are
absent, so the resulting app is a validation artifact, not a distribution build.
The framework tests use controlled native identity and acknowledgements; they
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
