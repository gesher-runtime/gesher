# Round 2 continuation status and validation

Date: 2026-11-23. Account: `palmoni5`.
PR: [#391](https://github.com/gesher-runtime/gesher/pull/391).
Round 1: [#388](https://github.com/gesher-runtime/gesher/pull/388).
Base: `bcfb5044029d486e17138dc417f3ced81524cf3f`.

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
| Actual-framework activity usage / usage-query | 41 / 901,153 checks pass |
| Query model | 12,655 checks pass |
| Usage observer helper | 1,117 checks pass; full service boundary and actual ATM callsite tests pass |
| Surface geometry / release / damage | 158 / 38 / 75 checks pass |
| Layered / presentation allocations | 1,441 / 38,028 checks pass |
| Frame recycling / video scaling | 331,204 / 32,165 checks pass |
| Readback / display refresh / mouse | 12,000 / 44 / 25 checks pass |
| Prepared worker, storage-query and AndroidX Java | Earlier component run: 64 checks pass, including actual Android JSON; this did not establish worker availability |
| Prepared native worker ownership / timeout / completion | Earlier component run: 314 checks pass with ASan/UBSan; missing-denial negative control rejected; this did not establish faithful network errors |
| Existing WebStorage deletion / boundary / owned streams | 16 / 32 / 17,352,558 checks pass |
| Production native compilation | Relevant graphics/window and complete WebView host units compile for Windows x64 and ARM64; complete patched-JNI builtin/shared units pass on x64. ARM64 patched JNI has the pre-existing `sysv_abi` limitation detailed below |

Existing CI entry points now invoke the new observer, refresh and worker suites;
no workflow files are modified. Compiler/sanitizer settings propagate to the
native worker suite, including its negative control.

Graphics ABBA measured CPU copying over 400 1080p frames. The 64x64 dirty path
took 2.408 / 2.368 ms in the two candidate passes versus 980.107 / 886.432 ms for
the baseline full-copy path. Checksums matched. Full-frame timing was noisy and
does not establish a speedup. Usage's workload schedules one deadline for 1,000
observers and 10,000 duplicate lifecycle events, with no inactive polling.
No performance comparison against a no-op worker/storage/observer baseline is
claimed.

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
