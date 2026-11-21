# Unity-rate audio mixing: span work instead of per-frame bookkeeping

## Scope and contract

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

`Track::MixInto` previously repeated phase flooring, source availability checks
and an intermediate channel-frame copy for every output frame, including the
common case where the source and output rates match. The change processes
already-buffered integral-phase unity-rate samples in spans of at most 256
frames. It uses the existing history, source reads, ring, locks and lifecycle.
No new storage, cache, worker, device setting or capability is introduced.

The [Android AudioTrack contract](https://developer.android.com/reference/android/media/AudioTrack)
requires streaming stop to drain the written data, pause to retain playback for
resume, and static playback to honor its sample/loop position. The implementation
preserves those behaviors and all supplied PCM, channel downmixing, volume ramps,
source/played positions, callback order and error results. Neither the sinc
resampling kernel nor its coefficients change.

The original 256-frame source reads remain at the same boundaries. A span stops
at the requested output limit, currently available history or the known real
end. Volume and floating-point phase still advance once per output sample in
their original order. Batching is limited to integral phase below `2^52` and
256 frames, where every addition of one remains exact. A fractional phase from
a prior rate/speed/device change uses the scalar path: near a power of two,
`floor(t + 1)` can differ from `floor(t) + 1` after floating-point rounding.

All synchronization, drain, starvation, compaction and callback publication
remain in their existing positions. This is runtime work reduction, with no
substitution of an application's input, requested operation or success result.

## Deterministic work measurement

The unchanged fixture executes the actual production `Track` methods. It
observes the source availability evaluation and `ReadSource` call site; it does
not replace their logic. Each workload is **1,000 warmed 480-frame blocks** at
48 kHz, for one, two, eight or 24 channels.

| Work per workload | Baseline | Change |
| --- | ---: | ---: |
| Source-availability evaluations | 480,000 | 2,750 |
| Source reads | 1,875 | 1,875 |
| C++ allocations during warm mixing | 0 | 0 |
| Cumulative allocated bytes during warm mixing | 0 | 0 |
| Output source frames | 480,000 | 480,000 |

Availability evaluations fall by **99.43%**, while source read-ahead and produced
frames are identical. The four deterministic work budgets reject the baseline
with exactly four failures; its remaining 6,878 behavior checks pass. The changed
source passes all 6,882 checks. The earlier zero-allocation mixer contract is
preserved; this change does not claim a reduction in process RSS or retained RAM.

## CPU measurement

Linux x86-64 host, reported CPU AMD EPYC 9V74, Debian GCC 14.2.0,
`-std=c++20 -O2 -fno-exceptions`, 480-frame output blocks. Each case warms 100
blocks. Each revision is then run seven times, alternating the order. Timings
use `CLOCK_THREAD_CPUTIME_ID`; benchmark binaries have no availability/read
instrumentation or sanitizers. Every repetition verifies matching output
checksum, source position and frame count. Source conversion and mixing are
included; the streaming case also includes ring replenishment. Windows device,
WASAPI and ART work are outside this host measurement.

| Workload, output 48 kHz | Measured output frames per round | Baseline median CPU | Change median CPU | Baseline / change |
| --- | ---: | ---: | ---: | ---: |
| Static mono, 48 kHz source | 14,400,000 | 79.419 ms | 26.940 ms | 2.95× |
| Static stereo, 48 kHz source | 14,400,000 | 88.639 ms | 37.562 ms | 2.36× |
| Streaming stereo, 48 kHz source | 14,400,000 | 95.972 ms | 37.414 ms | 2.57× |
| Static eight-channel, 48 kHz source | 14,400,000 | 191.981 ms | 102.398 ms | 1.87× |
| Static stereo, 44.1 kHz source | 2,880,000 | 307.086 ms | 293.566 ms | 1.046× |
| Static stereo, 96 kHz source | 2,880,000 | 563.950 ms | 584.041 ms | 0.966× |

The first resampling control showed a 3.6% increase at 96 kHz. One focused
seven-round repeat, pinned to one allowed host CPU and using the same reviewed
production source, measured 623.733 ms before and 601.015 ms after at 96 kHz;
44.1 kHz measured 284.752 ms before and 288.457 ms after. The static unity-rate
stereo control still improved from 93.974 ms to 37.798 ms (2.49×). The resampling
results change direction between runs, so no resampling speedup or regression
is established by these timings. They are observations on this shared host,
not a CI timing threshold. A scratch-only loop-specialization experiment did
not provide a consistent gain and was not included in the change.

These are speedups of the measured mixer workloads. They are not application
startup, total process CPU, end-to-end latency or device underrun measurements.

## Verification and its boundaries

- **6,882 new production-source checks pass with ASan/UBSan.** Independent
  supplied-sample/channel oracles cover every PCM format, 1/2/3/8/24 input
  channels and 1/2/4 output channels. State transcripts cover static/streaming
  playback, irregular/zero/long blocks, volume changes, three rate-change
  mechanisms, finite/infinite loops, ring wrapping, starvation, frozen stop
  boundaries, pause/resume, flush/reload/seek, release and ordered callbacks.
- **284,559,164 bytes** of output PCM, source/played state, retained history and
  callback transcripts match the exact baseline byte for byte. Wall-clock
  readings, object addresses and struct padding are deliberately excluded.
  The coordinator independently reproduced this complete sanitizer comparison
  on the reviewed implementation.
- Explicit one-ULP-below-integer phases at 2, 4, 16, 256 and 4,096 verify repeated
  floating-point additions against a scalar phase oracle. This prevents an
  otherwise plausible batching change from skipping a source sample.
- Existing Track lifecycle **162**, SoundPool **10**, and rate-history
  **32,720** checks (360 cases) pass with ASan/UBSan.
- Existing mixer allocation/byte-comparison suite: **480 changed-source checks**
  and **420 baseline checks** pass with ASan/UBSan; all **27,126,900 output and
  position bytes** match. This preserves its all-format/channel/rate allocation
  budgets.
- Complete production `engine.cc` compiles for Windows **x64 and ARM64** using
  CI-pinned LLVM-MinGW `20261006`, `-Wall -Wextra -Werror -fno-exceptions`, the
  repository's math/platform definitions and the actual pinned Android logging
  header. This is normal-runtime source, not standalone logging or a substituted
  Windows header.
- The new host regression is wired into the unconditional CI preflight next to
  the existing mixer allocation test. `git diff --check` passes.

The host adapter drives Track synchronously instead of opening WASAPI, and
uses declared Engine attachment, clock and condition-variable scheduling
boundaries. Local ASan runs disable only LeakSanitizer's unavailable process
inspection under ptrace; ASan/UBSan remain fatal and CI uses normal defaults.
Fresh Windows/ART, physical audio endpoints and application workloads remain
pending. Cross-compilation does not establish their runtime results.

## Reproduction

From the repository root, with a host C++20 compiler:

```sh
gesher_perf_base=f93a90fac55dbe1aff641e6e3f26879292907cb2
python3 tools/test_audio_unity_rate.py --sanitize --compare-ref "$gesher_perf_base"
python3 tools/test_audio_unity_rate.py --source-ref "$gesher_perf_base"
# The previous command must fail exactly the four work budgets.
python3 tools/test_audio_unity_rate.py --compare-ref "$gesher_perf_base" --benchmark --rounds 7
python3 tools/test_audio_mixer_allocations.py --sanitize --compare-ref "$gesher_perf_base"
python3 tools/test_audio_state_contracts.py --sanitize
python3 tools/test_audio_rate_history.py --sanitize
```

`--measure-only` disables only the four work budgets. `--source-ref` changes
production input, never the fixture's assertions. On a ptrace-hosted executor,
set `ASAN_OPTIONS=detect_leaks=0` only for the local sanitizer commands when
LeakSanitizer cannot inspect the process.

Full production cross-compilation, with the pinned toolchain in
`gesher_llvm_mingw` and the actual logging includes in `gesher_log_include`:

```sh
for gesher_arch in x86_64 aarch64; do
  "$gesher_llvm_mingw/bin/$gesher_arch-w64-mingw32-clang++" \
    -std=gnu++20 -O2 -Wall -Wextra -Werror -fno-exceptions \
    -DGESHER -DUSE_MINGW -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI -D_USE_MATH_DEFINES \
    -Icompat/posix/include -isystem "$gesher_log_include" \
    -c compat/media/audio/engine.cc -o "engine-$gesher_arch.o"
done
```

Logging includes came from the official
[`android-16.0.0_r4` liblog include archive](https://android.googlesource.com/platform/system/logging/+archive/refs/tags/android-16.0.0_r4/liblog/include.tar.gz),
SHA-256 `569241e00e72ea5a98bf5c5527c7653562552833de95f6d8b0c7111c4bbe4294`.

Reviewed source identities:

| Input | SHA-256 |
| --- | --- |
| Baseline `engine.cc` | `d27529c594f3455c48b244a011c24908a28d5148b4f1b8ef3dd8ad0fefd0ccac` |
| Changed `engine.cc` | `2960f2c31583befa52564621ce04e654e72c6c0ff137721db93ee608464585da` |
| Changed production source plus unchanged header, as printed by the host generator | `5a58dcb718d65c4b7d0b5b79c7250adc71551c2e1eea15185848938ccd2e126f` |
| New fixture | `c460f308fe6a9b3c1bd2d53715a28af27e972c7c5d4a786a1c0b1f0c84517a0e` |
| New runner | `bae9972425f509e4a3f07b315b1ab9b7ebbb2554855781b1ed319c6228ac4689` |
