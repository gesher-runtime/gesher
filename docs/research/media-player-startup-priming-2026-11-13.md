# MediaPlayer startup priming

## Observed CI failure

Combined candidate `6c8ca61c4d7024c63ca6019aed6b3c58d13827a9` passed its
independent source reviews, focused checks, framework contracts, packaging/E2E
and ARM64 Windows build. Full [Windows 2022](https://github.com/gesher-runtime/gesher/actions/runs/43405125966/job/131829825628)
reported zero failed tests. Full [Windows 2025](https://github.com/gesher-runtime/gesher/actions/runs/43405125966/job/131829825568)
reported one: sound 11, MediaPlayer playing `tone_aac.m4a`.

| Measurement | Windows 2022 | Windows 2025 |
| --- | ---: | ---: |
| Frequency, target 587 Hz | 587.5 Hz | 571.8 Hz |
| Duration | 0.815 s | 0.835 s |
| RMS per channel | 0.350 | 0.346 |

The unchanged acceptance limits are 2% frequency, 60 ms duration and 15% RMS.
SoundPool supplies sounds 22–23 and was not the failing tone. New timer,
event-loop, AudioTrack state, surface geometry, Codec and InputSurface gates
passed on both systems.

That job retained text results but did not upload the media WAV. Its exact
interior waveform and the cause of that particular sample are therefore
unproven. The controlled reproduction below establishes a source defect
independently of the lost recording.

## Missing startup protection

MediaPlayer created a 250 ms PCM ring and enabled consumption before decoded
PCM was queued. It then decoded and wrote individual packets. The mixer could
exhaust the first packet before the decoder supplied another; nominal ring
capacity alone does not protect that startup.

Android permits an early MediaPlayer sink start because its downstream
AudioTrack supplies initial-fill protection. Pinned Android 16 r4 references:

- [AudioFlinger Track::isReady](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-16.0.0_r4/services/audioflinger/Tracks.cpp#1368)
  gates ordinary initial consumption on the configured fill level, with
  forced-ready and stopping exceptions.
- [AudioTrack threshold contract](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/java/android/media/AudioTrack.java#2344)
  and [playback documentation](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/java/android/media/AudioTrack.java#2977).
- [NuPlayerRenderer early sink start](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-16.0.0_r4/media/libmediaplayerservice/nuplayer/NuPlayerRenderer.cpp#1873).
  Calling sink start before decoding is complete is not itself the incompatibility.

The independently retrieved `Tracks.cpp` SHA-256 is
`52fa9cfb8b1248eb52417bfba314351a658d683fb4d3e035e678fcd821b84f92`.
The affected player/decoder/probe/test path and complete `Track::MixInto`
method were unchanged across both isolated PRs and the first combined candidate.

## Deterministic reproduction

The independent diagnostic uses actual production Track methods and the exact,
unmodified `media_probe Analyze` function. Input: 44.1 kHz stereo, 587 Hz,
35,280 source frames. Supply one AAC-sized 1,024-frame packet, delay the next
supply until 44 ms, then supply the remaining packets. Advance mixer requests
explicitly in 1 ms steps, without scheduler-dependent sleeps.

| Startup policy | Longest interior zero run | Underruns | Unmodified analyzer |
| --- | ---: | ---: | --- |
| Immediate consumption | 1,010 frames / 21.042 ms | 1 | 572.6 Hz, 0.821 s, RMS 0.349 |
| Fill the existing ring first | 1 frame / 0.021 ms | 0 | 587.5 Hz, 0.805 s, RMS 0.352 |

Both inputs contain 35,280 frames and both played counters report 35,280.
Counter equality does not establish lossless audible presentation: the measured
interior silence and underrun are the decisive observations. The original 2%
frequency limit rejects immediate startup and accepts priming. ASan/UBSan found
no errors in this diagnostic.

The analyzer merges silence shorter than 50 ms into the surrounding sound.
Its crossing-count calculation includes that silence in elapsed time, reducing
the measured average frequency. This explains the controlled result and is
consistent with the historical numbers; it does not reconstruct the missing WAV.

## Bounded repair

The change is confined to MediaPlayer playback:

- Prime initial playback, seek and completed replay using the existing ring.
- Retain readiness on ordinary pause/resume and seamless looping.
- Admit a full ring before waiting for the rest of an oversized PCM packet.
- Force-start and drain short EOS buffers under current pause/seek/shutdown
  state; restore the drain boundary when a paused EOF resumes.
- Hold audio-master video timing while the track is unprimed. Distinguish
  a stopped initial track from a completed drain; retain the video-only clock.
- Preserve the position-history segment covering the presented position and
  queued future loop transitions. Use the existing 64-bit timestamp frame count
  for media time and retirement, rather than the wrapping 32-bit playback head.

The short-loop requirement matters during priming: more than eight 20 ms loops
can be queued before playback begins. The previous hard cap could discard the
segment covering frame zero and yield a negative media position. Retiring only
superseded segments preserves timing; using 64-bit presented frames also keeps
retirement working past the 32-bit wrap boundary.

Generic Track behavior, the resampler, analyzer and acceptance limits remain
unchanged. Generic AudioTrack threshold behavior and the public threshold APIs
remain outside this repair's validation scope. The inspected JNI registration
table warrants a separate public SDK caller check for those entry points.

## Verification and retained evidence

`tools/test_media_player_priming.py --sanitize` is required in artifact preflight.
The host fixture compiles the complete production Playback class and the actual
Track state, ring and resampler methods. Decoder/reader, renderer, engine
attachment and event-delivery seams are controlled; condition-variable wait
observation permits deterministic interleavings. This is not a Windows codec or
device model.

Independent ASan/UBSan validation of the final source passed **158 assertions**.
These cover
short and empty EOS, oversized writes, pause before and during EOS drain,
ordinary resume, seek/replay, pending-seek admission at write/EOF/drain
boundaries, more than eight queued short loops, repeated zero-output loops,
media time and segment retirement beyond the 32-bit frame wrap, A/V admission
and longer video tails, video-only playback and cancellation. The captured
startup output has zero underruns and at most one consecutive interior zero
frame; the original analyzer reports 587.5 Hz, 0.805 s and RMS 0.352. Played-frame
equality assertions describe counters, not bitwise recovery of resampled audio.

| Compiled control | Outcome |
| --- | --- |
| Original `6c8ca61` production source, final fixture | 152 assertions reached; 23 behavioral failures and the unchanged tone gate fails at 572.6 Hz, 0.821 s, RMS 0.349; one underrun and 1,010 interior zero frames |
| Remove only deferred-start video guard | 158 assertions reached; three expected A/V failures |
| Remove only resumed-drain Stop | 155 assertions reached; one expected resumed-drain failure |

All three controls were independently compiled and executed with the final
fixture. The fixed fixture exits 0; each control compiles and exits 1 on its
behavioral assertions, without a
missing file/symbol, compile error or timeout standing in for the intended
failure. Local ASan used an external `detect_leaks=0` setting because the
workspace runs under ptrace. The driver and CI retain ordinary sanitizer
defaults.

The complete production `player.cc` also compiles without diagnostics for actual
LLVM-MinGW Windows x86-64 and ARM64 targets, with the repository's CMake
definitions, production logging, GNU C++23 and warnings as errors. Object
inspection confirms `coff-x86-64` / `x86_64` and `coff-arm64` / `aarch64`. The
only external AOSP header needed by this translation unit is the unchanged
`android/log.h` from the repository's Android 16 r4 pin; all other dependencies
resolve within the real repository or cross-toolchain. This qualifies the
translation unit's compilation, not linking or Windows execution.

Source and fixture SHA-256 fingerprints:

| File | SHA-256 |
| --- | --- |
| `compat/media/player/player.cc` | `cef194ae46c5df0b9d8eee4371c4a31f34e98cbdb05020a3f8e901e59f67e00a` |
| `tests/media/player_priming_host.cc.in` | `4adecb3ee22e7bb986e9fa3d893240c12195288dceb629fdbd09ca4b37edcc79` |
| `tools/test_media_player_priming.py` | `1fde0e88641ce4f4f31d68986c1598b853215b285ec63c53f4dea34d5266c316` |

Actual Windows mediaapp playback remains the complementary Media Foundation,
MediaPlayer and engine gate, with the original expected tones and limits. Full
jobs now retain synthetic WAV files, analyzer sidecars and application output as
`media-output-windows-2022` and `media-output-windows-2025`, including failures,
for seven days.

Independent source/host acceptance and fresh exact-head full Windows, framework,
E2E and ARM64 results are recorded in [PR 14](https://github.com/gesher-runtime/gesher/pull/14).
A pending or failed full runtime gate blocks merge. This is not physical
audio-device qualification or a guarantee of universal Android media parity.

## Coexistence with concurrent work

Open PR 15 at `d2e11ea48580e46cba48156aafa8308476e39813` has no changed-file
overlap with this PR. PR 16 at `6bda09e320753cf937f0d8b4c56b9b680043d7c4`
has distinct native, build and workflow changes, with no Player implementation
or priming fixture overlap. Independent dry merges found one avoidable textual
conflict where both PRs inserted preflight checks at the same location. Moving
the intact Player check before the existing host-event-loop step makes both
pairwise merges and the three-PR union clean. Both sets of checks, timer/futex
targets and media capture retention remain present exactly once. This is source
coexistence evidence at those heads, not combined runtime qualification for the
other PRs.
