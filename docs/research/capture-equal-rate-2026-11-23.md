# Equal-rate capture processing — 23 November 2026

This is an issue #56 **T12 component leaf**. It reduces work in the existing capture stream when the source rate and channel count already match the requested stream. It does not complete an audio routing, device, API-family or performance-budget roadmap row.

## Production change and compatibility boundary

`CaptureStream::Push` previously walked the resampling phase and copied current/previous channel frames for every input frame, including frames discarded because the capture ring was full. For equal rates/channels and an integral zero phase, those coordinates already identify contiguous input. The new path reads that input directly, retains the prior block's last frame for the first output, and advances the entire input block's phase/history/lost-frame accounting after processing the admitted prefix.

The shortcut requires equal rates, equal channels, positive-zero phase, at most `2^52` input frames and `FE_TONEAREST`. Other inputs use the original resampling loop. The bound keeps the original repeated additions of one exact. The common phase subtraction, final history update, PCM conversion, ring publication, marker/period events and lifecycle remain in their original positions.

The arithmetic remains `previous + (current - previous) * 0.0f`. A direct copy would change infinities, NaNs and signed zero. The initial draft's directed-rounding test found a real signed-zero mismatch under `FE_DOWNWARD`; that draft's source hash and failure are retained in the raw evidence. The final rounding guard preserves the original loop in all nondefault rounding modes. The existing one-frame interpolation history is preserved, including the initial repeated sample; changing that signal behavior is outside this optimization.

No source is replaced, no capability is added, and the runtime still selects the actual WASAPI input through the existing source/permission/lifecycle path. [AudioRecord's documented read contract](https://developer.android.com/reference/android/media/AudioRecord) returns complete frames and defers a terminal dead-object error when a read has already transferred data. The unchanged capture/JNI/NDK boundaries continue to own those contracts. This change adds no heap buffer or retained resource; the existing ring and conversion scratch retain their existing bounds.

## Complete component measurements

Baseline: published main `5aa6b39027e9c38de2781ff9cda066418d25b1ba`. The candidate production SHA256 is `fa8e155bcedf5be50c6606f7c12032fed95cb01dcc7bb2e25d29f62d3a3306d7`.

Eight fresh-process ABBA blocks retain all **32 processes / 288 workload observations**, 16 observations per variant/case. The host reports Intel Xeon Platinum 8573C; Linux x86-64, GCC 14.2.0, `-O2`, ordinary allocators, no source-work counters or sanitizers in timed binaries. All other agent lanes confirmed idle for this final window. CPU uses the process CPU clock; wall time uses the monotonic steady clock. All first-call, small-stream, fallback and adverse samples are retained.

| Workload | Baseline CPU ms | Candidate CPU ms | CPU change | Wall change |
| --- | ---: | ---: | ---: | ---: |
| cold-stereo | 0.070126 | 0.077055 | +9.88% | +48.44% |
| small-streams | 15.177171 | 14.286480 | -5.87% | -6.23% |
| mono48-s16 | 126.351741 | 81.452560 | -35.54% | -36.17% |
| stereo48-s16 | 206.174756 | 146.977017 | -28.71% | -28.50% |
| eight48-s16 | 226.395553 | 177.285540 | -21.69% | -22.65% |
| stereo48-float | 130.941568 | 69.978226 | -46.56% | -48.08% |
| resample44-control | 64.257794 | 64.430492 | +0.27% | +0.31% |
| downmix-control | 128.103344 | 130.339540 | +1.75% | +2.64% |
| overflow-stereo | 24.242863 | 4.889466 | -79.83% | -79.84% |

Each timed workload opens/parses an actual explicitly selected float WAV, starts its unchanged paced source and a capture stream, repeatedly calls source `Pull`, `Push` and `Read`, collects final counters and stops/releases/destroys the objects. The file has 4,093 frames, so normal 480-frame blocks also exercise source wrapping. Each due block is driven by an explicit source-clock input advanced by 10 ms. Measured wall time is processing time; it does not include waiting for real capture periods or describe live-device delivery latency.

The actual complete `CaptureStream` member bodies, PCM conversion and paced WAV-source sections are extracted unchanged into a separate generated translation unit. Only engine attachment and the source's input clock are supplied by the fixture; private visibility is exposed for state assertions. This Linux measurement does not execute the original whole `CaptureEngine`/WASAPI translation unit, Java/ART, JNI/NDK adapters, physical-device scheduling or an application. WAV creation/input generation, output-buffer preallocation and full correctness-oracle work are outside the timed interval. WAV loading and all production stream/source storage destruction are inside it. The input file is freshly written before the clock and can be cached by the OS; the first case is not a disk or application cold start.

Normal mono/stereo cases process 30,000 ten-millisecond blocks per stream; the eight-channel, resampling and overflow cases process 10,000. `cold-stereo` opens one stream for one block; `small-streams` opens and destroys 1,000 separate one-block streams. The 44.1-kHz resampling and stereo-to-mono cases retain the original loop. The overflow case admits 37 of each 480 frames, preserving all 443 discarded frames in the lost count and the final source frame in history.

The first single-stream CPU median rises from **70.126 to 77.055 µs** (+6.929 µs, +9.88%); wall time rises from **58.585 to 86.962 µs** (+48.44%). The raw ranges overlap widely: baseline/candidate CPU 57.574–702.446 / 52.737–678.279 µs, and wall 50.422–732.665 / 46.078–757.250 µs. Clock bookends and first-use host-library costs are material at this scale. The 1,000 independently created one-block streams improve by 5.87% CPU, but that does not erase the adverse first-stream result. Its cause and statistical significance remain unresolved. The earlier four-observation-per-variant pilot had a +44.72% cold CPU median and is retained separately; it is not substituted for the final run.

The unchanged resampling/downmix controls show +0.27% / +1.75% CPU medians. The long workloads also contain overlapping samples; no per-call p95/p99, universal speedup or approved release-regression budget is inferred. The first-stream medians exceed the 5% magnitude discussed in [issue #178](https://github.com/gesher-runtime/gesher/issues/178); its requirement concerns statistically meaningful regressions and explicit release budgets, which these noisy host samples do not qualify. Representative Windows/application measurement and budget acceptance remain open.

## Work and storage accounting

After priming, an admitted 480-frame block previously called the channel-frame helper 960 times. The candidate calls it once for final history; the direct interpolation and PCM work still execute for every admitted sample. A 12-block, equal-rate/channel source run goes from 11,521 calls to 13, including initial priming. These are source-helper observations in a separate instrumented build, not counts of machine instructions or measured CPU cycles.

The existing zero-allocation warm `Push`/`Read` budgets still pass. The optimization does not claim a reduction in allocations, peak simultaneous storage, process RSS or private commit. It retains the original conversion-vector reservation and release behavior.

## Correctness and execution evidence

- The new actual-source ASan/UBSan corpus passes **116,512 candidate checks**, **116,509 baseline checks** and an exact **1,658,801-byte** full sample/state/callback transcript comparison. The three additional candidate checks enforce the new helper-work budget. Counts include individual record/guard assertions, not that many independent scenarios.
- Independent finite PCM oracles cover all five storage formats, mono/stereo/eight channels, one-frame and odd capacities, partial reads, input/output guards, ring wrapping, zero input and overflow. Explicit fallback cases cover channel mapping, different rates and fractional phase.
- A float corpus crosses 16 bit patterns, four rounding modes and three phase states, including positive/negative zero, subnormal and normal bounds, infinities, quiet/signaling NaNs and payloads. The independent scalar oracle checks finite values and signed zero; the exact baseline transcript additionally checks all emitted NaN bits and raised floating-point flags.
- Source-rate/channel transitions, stale generations, stop/restart, source failure, queued-data-before-error behavior, release/idempotence, timestamps, ring/history state and callbacks are retained in the transcript. The existing broader corpus also adds matching-channel wrapped and nested callback `Push`/`Read`/`Release` cases.
- That broader actual capture corpus passes **5,262,544 candidate checks**, **5,262,532 baseline checks**, and an exact **14,804,794-byte** PCM/state/callback transcript comparison. Its warm allocation and at-most-two-read-copy budgets remain mandatory.
- Nine compiled production mutants fail their named assertions: disabled shortcut; missing channel/rate/fractional-phase/negative-zero/rounding guards; wrong prior history; lost discarded-frame accounting; and copying the previous value instead of preserving zero-weight arithmetic. No compile failure, crash, timeout or sanitizer failure is accepted in place of the intended assertion.
- An independent agent reviewed the complete `Push` and lifetime boundaries and reproduced the new sanitizer corpus, complete baseline transcript and all nine mutants. The temporary `current_frame_`/`previous_frame_` arrays remain private scratch; the fallback overwrites them before using them.
- The **entire original `capture.cc`** compiles in ordinary runtime mode with the actual pinned AOSP log header and `-Wall -Wextra -Werror` for Windows x64 and ARM64. The new component fixture also compiles and links for both ABIs. This is cross-compilation, with actual Windows/device execution still pending full CI.
- The sanitizer/mutant helper is unconditional in `ci.yml`; the new native component executable is mandatory in the existing Windows media track. All original real-device-or-explicit-unavailable, AudioRecord, AAudio, OpenSL and recording gates remain. The branch retains current main's published `92c87bef013c0f1a5423b49b68d9b78079a8acd9` reusable-framework reference and exact recipe blob `6dc4cca565aaf15fdf518eee825b0e44765b275f`. This is the incoming maintainer-selected reference; its recipe is byte-identical to the measurement baseline's framework.

Local sanitizer runs set `ASAN_OPTIONS=detect_leaks=0` because this executor prevents LeakSanitizer's ptrace check. CI retains its normal sanitizer settings. Actual Windows runtime, physical audio, application timing, translated-ARM64 cost and release-budget acceptance remain open; this component result does not establish those outcomes.

## Current-main composition

Before publication, this leaf was composed with main `fdf4861b608d7a5fe8c0fa6c8a285dfbdf573d91`, including the shared-stdio and retained-producer changes. The measured baseline remains `5aa6b39027e9c38de2781ff9cda066418d25b1ba`. Exact source comparison confirms that the complete production `capture.cc`, all new and extended capture fixture sources, both Python helpers, the native target definition and the Windows media track are byte-identical to the measured and independently replayed feature checkpoint. Their complete SHA-256 identities are retained in the raw report. No timing sample or transcript was replaced, and no fresh integrated timing result is claimed.

The only workflow difference from current main is the added unconditional capture sanitizer/mutant invocation. Incoming framework reference `92c87bef` and all main gates remain intact. Fresh integration checks pass all 28 artifact-policy tests, six cache-policy tests, 26 source-transition negative controls, four repinned-property controls and 23 shared-stdio controls. Full Windows/device execution still requires CI on the published head.

## Reproduction

Use new output directories. [The raw report](capture-equal-rate-2026-11-23.json) retains complete observations, source/build/executable hashes, first-draft failure, the exploratory pilot and cross-compilation receipts.

```sh
python3 tools/test_audio_capture_unity.py --sanitize --negative-controls \
  --compare-ref 5aa6b39027e9c38de2781ff9cda066418d25b1ba --output-dir /tmp/capture-contracts
python3 tools/test_audio_capture_allocations.py --sanitize \
  --compare-ref 5aa6b39027e9c38de2781ff9cda066418d25b1ba
python3 tools/test_audio_capture_unity.py --measure --build-only \
  --source-ref 5aa6b39027e9c38de2781ff9cda066418d25b1ba --output-dir /tmp/capture-before
python3 tools/test_audio_capture_unity.py --measure --build-only --output-dir /tmp/capture-after
python3 tools/measure_audio_capture_unity.py --baseline /tmp/capture-before \
  --candidate /tmp/capture-after --rounds 8 --output /tmp/capture-abba.json
```
