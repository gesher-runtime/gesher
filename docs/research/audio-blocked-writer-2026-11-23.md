# Preserve prepared audio during a blocking write

This repairs the shared Track producer/mixer boundary found while monitoring issue #56 PRs. It does not attribute the problem to #353's metadata changes or claim a general CPU improvement.

## Failure and sufficient mechanism

The [Windows 2025 media job](https://github.com/gesher-runtime/gesher/actions/runs/45150068724/job/137649237541) reported **sound 7: 727.4 Hz, not 750**. Tested merge `498ed2a9fa8c9d63f0bb093f779463697d3a5715` has the same engine, sink, AudioTrack JNI, media app and oracle as the corresponding original main sources. Framework/API assertions passed; the recorded waveform failed the unchanged 2% frequency check.

Both original CI WAVs are retained byte-for-byte in the evidence archive:

| Artifact | WAV | SHA-256 |
|---|---|---|
| Failed Windows 2025, 14812914126 | audio-6000.wav | `6270d231645fbf270a990d73451db543a7312740e8698e01147eac3620608e3d` |
| Passing Windows 2022, 14814867651 | audio-1564.wav | `332be1c3ffe9b8e572278eece6aa9e6d13c7885583837850539d0f6f08517ca8` |

The failed tone has 14,846 frames instead of 14,400. Removing only relative frames `[4544,4990)` in memory leaves every stereo PCM byte identical to the passing tone. The usual upward-crossing interval is 64 samples; the interrupted interval is 510, exactly 64 + 446. This identifies a **446-frame/9.292 ms interruption**. Neither original WAV is edited. Unchanged production Analyze reproduces 727.4 Hz and 750.0 Hz; the existing Windows threshold still rejects the interrupted original.

The default 48 kHz ring has 960 frames. ClockedSink may request catch-up chunks up to 4,800 frames. MixInto holds mu_ throughout its chunk. A blocking writer can already have its remaining samples prepared, yet cannot refill after ReadSource notifies it because the mixer still holds the mutex the caller must reacquire. The mixer can then report starvation and leave zeros.

A real waiting writer with 14,400 supplied frames and a 960-frame ring reproduces this on a 1,406-frame MixInto request: 960 source frames and 446 missing frames. The same actual-source fixture reproduces it on the tested merge, main `8f53b604` and #334 head `3e7afab2c5a88128e08a41e7273257d3a5f323ce`. #334 changes WASAPI notification/grace timing, while its capture sink and this refill boundary remain unchanged.

The original job did not record individual mixer chunks or scheduling history. This demonstrates a sufficient mechanism, not a reconstruction of that unrecorded trace.

## Contract and change

The [Android AudioTrack contract](https://developer.android.com/reference/android/media/AudioTrack.html) permits blocking streaming writes to wait for transfer, with a short transfer on interruption, pause/stop or error. Pinned [Android 16 r4 implementation](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaudioclient/AudioTrack.cpp) and [header](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libaudioclient/include/media/AudioTrack.h) are retained with hashes as contract references; no AOSP runtime differential is claimed.

Gesher obtains Java primitive-array storage before Write and releases it after the synchronous call returns. The [JNI contract](https://docs.oracle.com/en/java/javase/23/docs/specs/jni/functions.html) keeps that pointer valid until release. The direct-buffer argument and JNI bridge's shared Track reference also remain alive. The mixer receives only already-live source memory, never a JNIEnv or Java reference. Concurrent application mutation of its own input acquires no new guarantee.

A playing blocking writer now publishes a stack-local pending record only while waiting for ring space. When ReadSource exhausts the ring, it can commit that prepared input into the same ring under mu_. Common CopyToRing preserves capacity, current buffer limit, wrapping and whole frames. Conversion, resampling and output consume owned ring bytes as before. There is no mixer wait, extra worker/timer, clock change, synthetic input or suppression of genuine underruns.

All links, source accesses and progress share the mutex. The real wait reacquires it before stack cleanup. The writer counts exactly the bytes committed on its behalf; idempotent unlink handles mixer-completed records and interruption. No borrowed pointer survives Write's return. Metadata costs are two list pointers per Track and one stack record per waiting call; no heap queue or memory reduction is claimed.

Every assisted transfer checks the original interruption epoch. Pause followed by start cannot revive an old caller; stop freezes the already-committed drain boundary; partial interruption returns exact committed bytes. FIFO applies to currently registered records, not global fairness or atomic whole-call ordering. Static/nonblocking writes, inactive priming, partial-frame truncation and NDK/player feeder behavior remain unchanged.

## Original 982-based validation boundaries and results

The required host fixture compiles complete production Track members/header, PCM conversion and resampler through the existing audio-state generator. Engine attachment, clock transport and CV scheduling are explicit adapters. Full transfers/concurrent writers use real CV waits; the hook controls precise pause/restart and spurious interleavings. Actual pthread cancellation checks C++ scope cleanup. This does not execute the Windows engine thread, ART or a device.

The final ASan/UBSan corpus passes **1,892 checks**, covering:

- Rings of 1/7/17/960 frames, complete 14,400-frame transfers, unchanged ring allocation and retained output after caller input is freed.
- Two/eight/32 waiters, two partial callers, wrap and live limit shrink, spurious unregister/re-register, exact byte progress and cancellation unlink.
- Pause/stop/restart epochs and concurrent Release, which waits for the mutex protecting real source access before returning ownership.
- All five PCM formats, 1/2/8/24 channels and 24/48/96 kHz rates versus an actual Track already owning the full source. Complete PCM, position, callbacks, genuine underruns and bounded ring state are checked across mono/stereo output and varied chunks.
- Original static/nonblocking/inactive behavior and partial-frame truncation.

The fixture also executes unchanged CaptureSink Render, PCM rounding, WAV writer/destructor and the complete Analyze function. Only single-track Mixer attachment and group gain lookup are supplied; render requests are explicitly driven. This is not execution of the Windows capture clock/endpoint selection. Current output matches an independent integer PCM oracle and measures **750.0 Hz**. The unchanged-main fixture inserts 446 frames and measures **727.3 Hz**. This is a separate tone/placement from the original job's 727.4 Hz; both interrupted outputs fail the original 2% rule. Fixture-only producer scheduling between ordinary periods isolates one catch-up request; production gains no such wait.

Two unchanged-source controls fail their named continuity assertions. Seven compiled mutations fail only their named assertions: remove refill, accept an interrupted epoch, lose committed progress, omit cancellation unlink, ignore the current buffer limit, reverse waiting-record selection, or repeat consumed source. Compiler failures, sanitizer faults and crashes do not count as passing negative controls.

Existing audio-state/SoundPool suites pass 162 + 10 checks. The unity-rate suite passes 6,882 candidate and 6,878 baseline checks, with an identical **284,559,164-byte PCM/state/callback transcript**. These remain component results, not application/device equivalence.

Complete original/current engine runtime TUs and unchanged media_probe TU compile with pinned LLVM-MinGW, actual Windows/AOSP/project headers and `-Werror` on x64 and ARM64. The engine uses its real liblog branch. These six receipts prove compilation, not full linking or Windows execution. Local LeakSanitizer is disabled under this executor's ptrace environment; ASan/UBSan remain active, and CI sanitizer settings are unchanged. Original source/supplier gates pass, including 36 supplier metadata mutants and its independent hash control.

## Original evidence and CI

Base: `98215dd7e798090fb5ab6160490ae78f1793ab3a`. Baseline engine SHA-256: `2960f2c31583befa52564621ce04e654e72c6c0ff137721db93ee608464585da`; candidate: `fa2809a64a1340b7353cc365d2a91ed5b94e9729d115c2b87639bb1fdb821467`.

CI adds one unconditional source/negative-control invocation beside audio-state. Existing Windows media tests, sound counts, frequency/duration/RMS limits and deadlines remain unchanged. Full manual fork CI and actual Windows audio results remain required; no passing result is claimed here.

The commands below reproduce the final composed source; historical 982 commands and source snapshots remain in the original archive.

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tools/test_audio_blocked_write.py --sanitize --negative-controls \
  --output-dir /tmp/audio-refill
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tools/test_audio_state_contracts.py --sanitize
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tools/test_audio_unity_rate.py --sanitize \
  --compare-ref bcfb5044029d486e17138dc417f3ced81524cf3f
```

The [JSON index](audio-blocked-writer-2026-11-23.json) and [archive](data/audio-blocked-writer-2026-11-23.tar.gz) retain original WAVs/analysis, source and fixture snapshots, generated real-source programs, negative reproductions, final mutation logs, current/baseline capture WAVs, compile commands/header hashes and source-policy receipts. No timing benchmark or universal release-budget acceptance is claimed.

## Composition with current main

Before publication, main `bcfb5044029d486e17138dc417f3ced81524cf3f` merged #334's WASAPI group notifications and elapsed-time starvation grace, followed by #377's local reusable-framework workflow call. Feature `63fc00202d6d410f744be44853881e53478a0b76` and that main compose automatically in ordered merge `da4da4977a9e04fc82bce8aa4091931d06807850`, tree `f80ab91a34dee76b5bb33af245acbc30879572ce`. Independently reconstructing Git's canonical merge produces the same tree. The complete pending-record/copy/write and ReadSource bodies are byte-identical to the earlier reviewed feature; no production conflict was manually resolved.

The host fixture needs the incoming Sink virtual method's real default body for its vtable. The generator now includes that exact body and supplies an abort-only Windows wait boundary. Explicit capture Render calls never enter this wait; the adapter cannot claim a successful Windows wait. Paired candidate/reference mixers use main's existing nominal playback-clock adapter so both see the same elapsed-time starvation grace, including empty periods. Actual real-wait/concurrency tests keep their real CV behavior. No assertion or production sample is removed or substituted.

The required unchanged-source negative baseline now selects `bcfb5044`, which is in the final branch ancestry. A fresh ASan/UBSan replay again passes **1,892 checks**, both unchanged-current-main negatives and all seven compiled assertion-specific mutants. Current capture remains 750.0 Hz; current main's interrupted control remains 727.3 Hz and fails the same frequency predicate. The prior 982 results and original CI recordings remain separately retained.

Existing audio-state and SoundPool still pass **162 + 10** checks. Repeating the complete unity comparison against `bcfb5044` passes **6,882 / 6,878** checks and an exact **284,542,469-byte PCM/state/callback transcript**. Its size differs from the historical 982 transcript because both current versions include main's changed starvation grace; no cross-version transcript identity is claimed. Complete current-main baseline/current engine TUs and current-main media_probe compile strictly on x64 and ARM64 in six new receipts. No Windows runtime or complete DLL-link result is inferred.

Current source-policy checks pass 26 ordinary, four repinned-property and 23 shared-stdio controls; supplier qualification passes 36 metadata mutants plus its independent authority hash check. The fork diagnostic suite passes all 22 tests. The first diagnostic CLI invocation used an incorrect path and failed before execution; its error log remains beside the corrected passing invocation.

The final workflow differs from current main only by the additional required blocked-write invocation. All incoming local framework calls and gates are retained, and the Windows media track is byte-identical to current main. The JSON's separate `current_main_integration` entry and [integration archive](data/audio-blocked-writer-main-2026-11-23.tar.gz) carry these new source identities, exact commands, generated programs and logs. The original archive and every original JSON field remain unchanged. Actual Windows audio, cancellation/close behavior in the complete runtime and the full manually dispatched fork CI still require their real execution results.
