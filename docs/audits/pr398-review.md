# PR 398 review and contract corrections

Review date: 2026-11-24. Review and repair account: `palmoni5`.
Original head: `916510a3f2292431ef1d8d6183a7d59937996437`.

## Scope and retained implementation

The retained production change gives native audio permission checks scoped JNI
attachment ownership. Already attached callers retain their attachment; a native
caller attached by this check is detached on every completed path. Permission
decisions still come from the actual `AudioService` policy, and lookup or policy
failures deny access. It installs the recording permission provider during
registration. The current main branch already attaches native callers through
`AttachedEnv`, so this revision does not claim that all native capture on current
main is denied. The change concerns attachment ownership and its regression
coverage, not a replacement recording backend or a new permission default.

The record-lock addition tests the existing, unchanged production liveness
implementation, including modern/legacy file identity, error preservation and
process/thread/object pruning. No source qualification receipt is weakened or
repinned for it. The new runner is included in the native contract CI step;
audio and speech already run through the existing framework workflow.

## Speech findings and correction

The original speech changes were based on a wrong admission contract. They
changed invalid-provider errors, introduced a ten-session limit and sent a new
callback after a client token was already dead. The revised production broker
is restored exactly to the integrated main version. Tests retain the expanded
concurrency and cleanup coverage with an independently checked Android oracle.

The pinned source is Android `android-16.0.0_r4`,
[`SpeechRecognitionManagerServiceImpl.java`](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/speech/SpeechRecognitionManagerServiceImpl.java).
The decoded source SHA256 used for review is
`35f569892b2cc9aaef7ac406195aa9869b04b5ad789b7bed2155a2cfe5aa0e41`.
Its implementation establishes:

- No selected component reports `ERROR_CLIENT`.
- An invalid explicitly selected component fails service creation and reports
  `ERROR_TOO_MANY_REQUESTS`.
- The limit of ten counts distinct provider connections per UID. Multiple
  sessions share a component connection; the session count is telemetry, with
  a separate TODO for future session refusal. It is not a ten-session cap.
- When linking the already-dead client token throws, the broker cleans up and
  returns without a new callback.

The revised suite admits sixteen pending sessions and a seventeenth connected
session for one component; retiring some preserves its shared provider, and
retiring all releases it. Other cases exercise ten distinct providers, UID
isolation, repeated invalid components, silent token/owner death and cleanup
without leaked quota. A worker barrier makes absence-of-callback assertions
deterministic. The revised test fails against the original PR source at its
invalid-provider assertion, and passes against the restored broker.

## Local evidence and limits

The repaired broker host suite passes 246 contract checks, 129 dispatch-budget
checks and 1,024 quota-budget checks. Native audio admission passes 147 checks
with address/undefined-behavior sanitizers, plus eleven real-JVM attached and
detached-thread checks with JNI checking enabled. The complete production
record-lock liveness host suite passes 122 checks with sanitizers.

The fixtures explicitly replace Android Binder/identity and Win32 boundaries.
They do not establish execution on ART, real Windows filesystem handles or
physical recording devices. Full framework/Windows CI belongs to the exact
published revision. Local LeakSanitizer cannot initialize under the container
tracer; local runs disable only leak detection, retaining ASan, UBSan and
explicit lifetime assertions. CI sanitizer settings remain unchanged.
