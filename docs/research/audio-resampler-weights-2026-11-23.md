# Exact repeated audio-resampler coefficients

This change reuses coefficients for exact integer or half-integer resampling
steps within one `Track::MixInto` call. The four eligible complete-Track workloads
use **35.09–43.07% less steady caller CPU** in the first six-block campaign. It
does not round arbitrary phases, retain coefficients between calls, change source
PCM, or alter the existing accumulation order.

There is a resource tradeoff: **1,024 bytes of coefficient storage on the stack**.
Compiler frame size increases even on paths that do not reuse coefficients.
Controls and first-lifecycle measurements are mixed; all results, including
adverse ones, remain in this report and its evidence archive. These measurements
do not establish an application performance budget, universal improvement, or
Windows/device speedup.

## Scope and current-main provenance

The runtime diff is confined to `compat/media/audio/engine.cc`: an exact-phase
classifier and coefficient calculation inside `Track::MixInto`. The API, source
samples, history ownership, locking, format, callbacks, timeline and lifecycle
remain in place. Complete unchanged-source comparison is the principal behavioral
oracle, supplemented by uncached scalar taps and real Windows engine assertions.

The measured public baseline is `bcfb5044029d486e17138dc417f3ced81524cf3f`.
The final branch starts from public main
`3b2eaca95b5ddd9cee3c3db4e430460e512eb355`, which adds 25 disjoint Java/shortcut
paths. All 14 production, fixture, helper, dependency and gate inputs in the
integration receipt are byte-identical across that advance. Measurements were
not rerun or relabeled as measurements of the unrelated new Java behavior.

A fresh changed-file census of all 77 open PRs found two `engine.cc` overlaps:
[#390](https://github.com/gesher-runtime/gesher/pull/390) changes blocked input
ownership in `Write`/`ReadSource`, and
[#232](https://github.com/gesher-runtime/gesher/pull/232) changes idle engine and
feeder work. Neither changes this coefficient calculation. Other media PRs add
their own Windows track 38 gates; this leaf does not claim their work or results.

A later focused review of new #398 (`916510a3`) and #399 (`5bffcb1e`) confirmed
no playback/resampler overlap. #398 changes recording-admission JNI attachment
and speech, with separate POSIX file-lock tests. #399 changes graphics, usage
and WebView paths. This review does not approve their unrelated implementations
or claim that they repair #390's playback mechanism.

This supplies bounded evidence for
[#56 T12](https://github.com/gesher-runtime/gesher/issues/56). It does not close
T12's application/ABI/power/latency matrix or implement M11's independent time
stretching, pitch control, MediaSync or general A/V synchronization.

## Exact arithmetic and resource ownership

The old loop computes `Kernel(table, (t - k) * cutoff) * float(cutoff)` for every
tap of every output frame. For an integer or half-integer effective step, an
actual phase of exactly zero or one half repeats. With `0 <= t < 2^51`, the
bounded integral taps and half-step differences are exactly representable;
repeating the phase repeats the same coefficient arguments.

Reuse requires a non-unity effective step for which `2 * step` has no fractional part,
the original half-width at most 64, and the current actual phase exactly zero or
one half within the stated bound. Two local arrays hold up to 128 coefficients
per phase. Each phase is filled when first encountered in this call. Original
tap traversal, per-channel multiply/add order and zero-weight exclusion remain.
An arbitrary phase left by another rate uses the original loop; if later
floating-point additions reach an eligible exact phase, that frame may reuse it.
All storage dies at return, so no coefficient crosses a change of rate, width,
history, thread rounding mode or Track owner.

`modf`, rather than an extra `floor`, tests the effective step. An early prototype
using `floor` added a floating-point exception flag on otherwise empty nonbinary
calls. The current full transcript checks final flags under all four rounding
modes. It does not claim equivalence of enabled floating-point trap timing.

For 31 mixes of 480 output frames, actual observed `Kernel` entry counts are:

| Effective step | Half-width | Baseline calls | Candidate calls |
| --- | ---: | ---: | ---: |
| 0.5 | 12 | 357,120 | 1,488 |
| 1.5 | 19 | 565,440 | 2,356 |
| 2 | 25 | 744,000 | 1,550 |
| 4 | 50 | 1,488,000 | 3,100 |

This reduces coefficient work; it is not an allocation-count claim. These
observers are absent from ordinary timing executables.

## Behavioral qualification

The host helper extracts complete actual Track members, declarations and audio
primitives. Existing Engine attachment/output-clock transport and immediate
callback delivery are explicit host adapters. Track uses its ordinary
`std::condition_variable`. Tests expose private state to inspect real history and
lifecycle. They do not execute the WASAPI engine loop, ART, hardware or an app.

The current ASan/UBSan run passed **8,816 candidate checks**, **8,284 unchanged
baseline checks**, and an exact **309,588,851-byte** PCM/state/floating-point-flag/
callback transcript comparison. An independent agent rebuilt and replayed the
same source with identical results. Coverage includes:

- Complete old-source output and retained state across streaming/static Tracks,
  five PCM formats, 25 format/channel pairs, rate/speed/output-rate changes,
  history compaction, ring resizing/wrap, gain, pause, stop/drain, flush, reload,
  release, marker/update/end/underrun events and timeline positions.
- An independent uncached scalar tap loop, all four rounding modes, signed zero,
  subnormals, infinities/NaNs, fractional inherited phases, one-ULP boundaries,
  widths 63/64/65 and channels 1/2/3/8/24.
- The actual classifier near `2^51`. This checks classifier/arithmetic behavior;
  it does not allocate a huge history or execute an entire mix at that position.
  Full old-source transcripts compare actual NaN bytes; the separate scalar
  oracle compares NaN classification.
- Empty/silent nonbinary calls preserve final exception flags; a nonfinite sample
  behind a zero-weight tap remains excluded.

Seven compiled negative controls must fail at a named assertion: disabled reuse,
conflated zero/half phases, admitted fractional phases, retained cross-call
weights, shifted taps, multiplication of a zero-weight nonfinite tap, and removal
of the phase bound. Compile failures, crashes or sanitizer failures do not count
as successful controls.

An initial focused fixture incorrectly assumed a fractional start could never
become exact after later additions. Its four failed assertions are preserved.
The corrected work oracle follows actual successive positions; independent
scalar and full unchanged-source comparisons remain strict. The abandoned
channel-register prototype and earlier coefficient-cache pilot, their sources
and every measurement are also retained.

## Ordinary complete-work measurements

Actual baseline/current production objects and one common driver are compiled
separately with GCC 14.2, `-O2`, no LTO, normal allocators, ordinary mutex/CV and
no coefficient counter. The adapter constructs the actual Track directly;
public `CreateTrack` admission/Engine registration and endpoint threads are
outside this host boundary. Input/output buffer preparation precedes the first
clock. Timed work includes owner construction, rate/gain/event setup, every
actual write/mix, complete output hashing and final-state checks, Stop/Release
and owner destruction. The first lifecycle includes existing lazy kernel-table
initialization when mixing is used.

Caller `CLOCK_THREAD_CPUTIME_ID`, enclosing `CLOCK_PROCESS_CPUTIME_ID` and
monotonic wall intervals are retained. Each process interval must cover its sole
timed thread's CPU interval. Every process verifies complete output/state and
retirement. Repeated lifecycles within a process are not independent samples.

The first campaign used six fixed ABBA blocks, 12 cases, 288 fresh processes and
576 first/steady observations. Each steady observation contains 64 complete
lifecycles. All three other agents held CPU-heavy work during
**16:02:42.618542–16:03:11.093311 UTC on 2026-11-23**.

Times are microseconds per complete Track lifecycle. Negative changes are reductions.

| Workload | First CPU change | Steady CPU before | Steady CPU after | Steady CPU change | Steady wall change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mono 44.1→48 kHz stream | -9.30% | 1780.074 | 1753.397 | -1.50% | -1.51% |
| Stereo 44.1→48 kHz stream | -2.17% | 1877.653 | 1968.014 | +4.81% | +6.12% |
| Stereo 96→48 kHz stream | -31.82% | 1687.818 | 1069.332 | -36.64% | -36.40% |
| Stereo 48 kHz, 0.5 speed stream | -30.34% | 3500.829 | 2272.260 | -35.09% | -34.78% |
| Stereo 48 kHz, 1.5 speed static | -32.34% | 1744.217 | 1108.506 | -36.45% | -36.44% |
| Stereo 48→48 kHz unity stream | +36.25% | 66.101 | 71.328 | +7.91% | +7.91% |
| 8-channel 44.1→48 kHz stream | -4.87% | 3317.761 | 3269.092 | -1.47% | -1.84% |
| 64-frame stereo 44.1 kHz stream | -14.37% | 9.409 | 9.968 | +5.94% | +8.51% |
| One-frame mono static | +14.08% | 0.813 | 0.816 | +0.32% | +0.30% |
| Stereo 192→48 kHz stream | -38.98% | 1735.654 | 988.083 | -43.07% | -43.07% |
| Stereo 288→48 kHz wide fallback | +1.49% | 1619.923 | 1640.181 | +1.25% | +1.37% |
| No-read static lifecycle | +0.16% | 0.205 | 0.208 | +1.47% | +1.24% |

The default unity control initially had a material adverse result: steady CPU
66.10→71.33 microseconds (+7.91%) and first CPU 339.62→462.72 microseconds
(+123.10 microseconds, +36.25%). Source/ordinary-object inspection found a larger
frame, changed register/code layout, two readiness bytes cleared per mix, and
once-per-mix `modf` on eligible-width nonbinary fallbacks. Unity skips `modf` and
coefficient work. The measured GCC object emits no bulk initialization of the
coefficient arrays or stack-probe loop.

### One predeclared longer control campaign

Before additional measurements, seven cases/cycle counts were fixed: unity
2,048; short stereo 4,096; one-frame mono 65,536; no-read 262,144; stereo 44.1 kHz,
wide fallback and the 96 kHz target 64 each. The same source, production objects,
common driver, executables, allocator and first lifecycle were reused. This
lengthens short control intervals without selecting a new source version or
discarding observations.

Twelve fixed ABBA blocks produced **336 processes / 672 observations** between
**16:21:43.151963–16:22:17.594200 UTC**, with all other agents CPU-idle. Both
campaigns remain intact in the archive.

Times are microseconds per complete Track lifecycle. Negative changes are reductions.

| Workload | First CPU change | Steady CPU before | Steady CPU after | Steady CPU change | Steady wall change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stereo 48→48 kHz unity stream | -6.51% | 66.066 | 66.456 | +0.59% | +0.58% |
| 64-frame stereo 44.1 kHz stream | +0.00% | 9.004 | 8.996 | -0.10% | +0.72% |
| One-frame mono static | -9.25% | 0.860 | 0.880 | +2.33% | +1.83% |
| No-read static lifecycle | -0.83% | 0.216 | 0.206 | -4.89% | -4.40% |
| Stereo 44.1→48 kHz stream | -2.51% | 1947.057 | 1931.667 | -0.79% | +0.45% |
| Stereo 288→48 kHz wide fallback | +0.94% | 1574.110 | 1616.798 | +2.71% | +2.72% |
| Stereo 96→48 kHz stream | -27.35% | 1696.141 | 1137.519 | -32.93% | -32.94% |

The earlier unity slowdown did not recur at the same magnitude: longer steady
CPU was 66.066→66.456 microseconds (+0.59%); first CPU was 375.42→350.98 microseconds
(−6.51%). This does not erase the first result or establish a regression bound.
Wide fallback remained +2.71% by median; one-frame steady work was about 20 ns
slower (+2.33%).

Block-paired results retain unfavorable variance. Longer short-stereo paired
mean CPU was +4.41%, with descriptive interval [+0.18%, +9.70%], despite its
near-flat median. Stereo 44.1 kHz paired mean was +16.19% [−1.25%, +39.67%],
despite its −0.79% median. Unity paired mean was −0.91% [−6.08%, +2.73%]. The
repeated 96 kHz target still reduced median CPU by 32.93% and paired mean by
31.32%.

Each paired percentage divides the mean of a block's two candidate CPU values
by the mean of its two baseline values, then subtracts one. Descriptive intervals
resample the six/twelve block ratios 10,000 times using
`random.Random(56).choices`, reset per case/phase, with sorted indices 250/9750.
They are not a prespecified release budget or a guarantee for another machine.
No observations were excluded and no further timing or source tuning followed.

## Stack and Windows limits

There is no new persistent Track allocation or coefficient-buffer ownership.
There is **1 KiB of automatic coefficient storage per mix call**. Strict complete
Windows translation-unit compilation with CI-pinned LLVM-mingw reports:

| Target | Old `MixInto` static frame | Current frame | Increase |
| --- | ---: | ---: | ---: |
| Windows x64 | 520 B | 1,576 B | 1,056 B |
| Windows AArch64 | 336 B | 1,408 B | 1,072 B |

Compiler frame estimates exclude callees; they do not measure committed stack
pages or process RSS. The GCC host object's own stack adjustment is 280→1,336 B.
The larger frame is reserved even when no reuse occurs. Six complete strict
Windows translation units pass: old/current engine and the new actual media
probe on x64 and AArch64. This is compilation evidence, not linking, native
ARM64 runtime, Windows scheduling or audio-device execution.

The existing mandatory Windows track 38 gains a separate `resampler-phases`
capture. It exercises genuine `CreateTrack`, complete static `Write`, Start,
position and callback delivery for half-speed stereo, 1.5-speed mono and
96→48 kHz stereo. Shared callback observations remain owned after timeout/Release.
All three source positions and exact marker/update/end counts must match. The
unchanged sound analyzer requires 2% frequency, 60 ms duration and 15% level
accuracy. Existing tones, gates and deadlines remain. No production samples are
fabricated or padded to satisfy that analyzer.

Actual Windows execution and the complete fork CI remain pending publication.
The host/cross-build results do not substitute for them. Local sanitizer runs
use `ASAN_OPTIONS=detect_leaks=0` because of this container's ptrace limitation;
ASan/UBSan remain enabled. The mandatory CI invocation does not change its
normal sanitizer environment and no successful LeakSanitizer claim is made.

The new host command is unconditional in the existing `artifact-preflight` job,
whose checkout retains full history for the reachable public baseline. Current
source-policy qualification passes its 26 mutation controls, four repinned
property controls and 23 shared-stdio controls. Supplier metadata checks pass
36 semantic mutations plus the authority-hash negative; these are metadata
checks, not native runtime results. Fork diagnostic tests pass 22 checks and
564 supplier-parity mutations. PowerShell parses the final Windows gate. No
workflow admission, supplier qualification, runtime assertion or deadline was
relaxed.

## Reproduction and evidence

From the repository root:

```sh
python3 tools/test_audio_resampler_weights.py --sanitize --negative-controls
python3 tools/benchmark_audio_resampler_weights.py --build-only \
  --output-dir ../media-resampler-weights-final-build
python3 tools/benchmark_audio_resampler_weights.py --run-only --blocks 6 --cycles 64 \
  --output-dir ../media-resampler-weights-final-build \
  --output ../media-resampler-weights-final-abba.json
```

The archived `media_resampler_long_controls.py` is the exact additional driver.
Place it beside a checkout named `gesher-media-resample-channels` and the fresh
build/first-campaign paths above, then execute it once. It refuses to overwrite
an earlier control result, checks all build hashes, and verifies each first
lifecycle identity against that campaign. Reserve quiet timing separately from
compilation and sanitizer runs.

The [JSON report](audio-resampler-weights-2026-11-23.json) identifies the complete
archive and per-member SHA-256 manifest. It includes source/generated/binary
hashes, raw stdout and all timing observations, failed/successful fixtures,
commands, cross-build/stack evidence, independent replay and the main/PR audit.
Large deterministic PCM transcripts retain their exact size/hash and
reproducible sources; the archive does not duplicate hundreds of megabytes of
identical output. Independent evidence review recomputed all 624 processes from the two final campaigns
 / 1,248 observations in 13,309 checks, including unchanged ordinary
binaries and first-lifecycle identities. No fresh timing or release-budget
acceptance is inferred from that review.
