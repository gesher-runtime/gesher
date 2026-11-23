# Reuse software audio decoder scratch

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), M04/M05/T12;
2026-11-23. Base `d38b5549dc3963f8d491e9205f9b2743bb6d3745`.

MP3, Vorbis and Opus allocated and initialized a maximum-size signed-16 PCM
vector on every `Decode` call. Each provider finishes writing the returned
samples before returning; the wrapper then copies those samples into the
caller's independently owned output. Keeping this private scratch vector in
each decoder removes the repeated temporary allocation without changing the
provider call, output length, PCM ownership or capability selection.

The complete nine-clip workload below reduces measured C++ allocation calls
by 48.6–49.8% and allocated-byte volume by 64.9–89.5%. Every complete output and
decode-after-drain output is byte-identical to the unchanged source. These are
allocation measurements of the real production decode/drain paths. They do
not establish a CPU, playback-latency, process-RSS or battery improvement.

## Behavior and storage lifetime

`compat/media/player/decoder.cc` is the only production file changed. Its MP3,
Vorbis and Opus implementations each retain one `std::vector<int16_t>` at the
same capacity they previously allocated per call. `Append` still copies only
the provider's valid output into the caller's vector. No pointer into scratch
is returned to callers, shared across decoder instances or handed to another
thread by this change.

The providers' existing flush/reset behavior and the wrapper's MP3 delay,
Vorbis page accounting, Opus delay and seek preroll remain unchanged. Flush
keeps the scratch allocation. Drain releases it; MP3 releases it before making
its existing 529-frame silence tail so that both scratch allocations do not
overlap. Destruction also releases it. Decode after drain allocates again and
preserves the provider's prior state and output. Reconfiguration constructs
a new decoder through the existing factory; no global pool or retained old
configuration is introduced.

The retained capacity between active packets is 9,216 bytes for MP3,
`16,384 * channels` for Vorbis, and `11,520 * channels` for Opus. This is an
explicit lifetime tradeoff: the baseline released this storage after each
call. In the measured workloads, peak simultaneous observed C++ storage is
unchanged, and scratch storage is zero after drain and destruction.

## Provider contract audit

All provider sources are the existing `android-16.0.0_r4` dependencies. No
codec, format, input, sample-rate, channel-count, error or EOS support is added
or removed.

* [PacketVideo MP3](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/module/codecs/mp3dec/src/pvmp3_framedecoder.cpp)
  bounds `outputFrameSize` by the supplied capacity before synthesis. The
  synthesis calls write the complete returned interleaved sample range. The
  wrapper still fills the existing recoverable-error output with silence and
  does not append the destination after fatal errors.
* [Tremolo](https://android.googlesource.com/platform/external/tremolo/+/bda690e46497e1f65c5077173b9c548e6e0cd5a1/Tremolo/dsp.c)
  bounds `vorbis_dsp_pcmout` by the supplied frame capacity and unrolls every
  channel into that returned range. It does not retain the caller's PCM
  destination. The wrapper still consumes only the returned frames and its
  current page's valid-sample budget.
* [libopus](https://android.googlesource.com/platform/external/libopus/+/15179bce8c2b7f35a99c810f4574560b99e90059/src/opus_multistream_decoder.c)
  copies each decoded channel into the caller's complete returned frame range;
  mapping value 255 explicitly clears a muted channel. A negative result is
  still an error, and the wrapper does not append output on that path.
* The fixture's Vorbis packet metadata follows the actual
  [AOSP OggExtractor page budget](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/module/extractors/ogg/OggExtractor.cpp).
  The valid-sample count belongs to the first packet completed on a page;
  subsequent packets consume the remaining budget. It is not fabricated as
  an independent per-packet trim length.

The test compiles the complete production decoder, all of the product's
selected MP3/Tremolo/libopus source units, and the actual AOSP `OpusHeader.cpp`.
The three provider call observers only poison/count the caller's output
storage and forward the real call. No provider result is simulated.
`AMediaFormat` reads and log transport are explicit host boundaries containing
the encoded headers and packet metadata. Unused Media Foundation selection
aborts instead of fabricating an available decoder.

## Complete-workload allocation measurements

FFmpeg generated nine 2.1-second clips from distinct channel signals with
silence and transitions, then independently decoded each clip. The workload
includes every compressed packet and final drain. Each call starts with an
empty caller output, as the packet-local PCM used by MediaPlayer and
MediaCodec does; allocating that output is included in the measurement.

The global C++ allocation observer records requested bytes, calls and live
ownership during those complete `Decode`/`Drain` calls. It follows deletion
after each call as well. Provider C allocations and decoder initialization,
fixture loading, metadata preparation and comparison storage are excluded.
No assertion that this is a whole-process heap or allocator-residency census
is intended. Storage poisoning is disabled for the measured pass.

| Complete clip | Packets | C++ calls, before → after | C++ bytes requested, before → after | Output PCM bytes |
|---|---:|---:|---:|---:|
| MP3 mono, 44.1 kHz | 82 | 166 → 85 | 945,698 → 199,202 | 188,928 |
| MP3 stereo, 48 kHz | 89 | 180 → 92 | 1,232,452 → 421,444 | 410,112 |
| MPEG-2 MP3 stereo, 24 kHz | 90 | 182 → 93 | 1,038,916 → 218,692 | 207,360 |
| Vorbis mono, 44.1 kHz | 106 | 211 → 106 | 1,921,924 → 201,604 | 185,220 |
| Vorbis stereo, 48 kHz | 115 | 229 → 115 | 4,171,520 → 435,968 | 403,200 |
| Opus mono, 20 ms packets | 106 | 212 → 107 | 1,424,016 → 214,416 | 202,896 |
| Opus stereo, 20 ms packets | 106 | 212 → 107 | 2,848,032 → 428,832 | 405,792 |
| Opus stereo, 60 ms packets | 36 | 72 → 37 | 1,242,912 → 436,512 | 413,472 |
| Opus six channels, 20 ms packets | 106 | 212 → 107 | 8,544,096 → 1,286,496 | 1,217,376 |

Observed peak C++ live bytes before/after are respectively identical for each
row: 11,520; 13,824; 11,520; 18,432; 36,864; 13,440; 26,880; 34,560; 80,640.
The last row retains 69,120 scratch bytes between active packets; the stereo
Vorbis and Opus rows retain 32,768 and 23,040 bytes. Every row returns to zero
observed scratch after drain.

Raw before/after counters, exact source/provider/fixture identities, output
hashes, oracle errors and negative-control diagnostics are in
[`software-audio-scratch-2026-11-23.json`](software-audio-scratch-2026-11-23.json).
The observer records real allocation requests; these numbers do not imply
that the operating-system heap returned or committed the same byte volume.

## Correctness and negative controls

The candidate passes 22,823 checks. A fresh build of all real provider units
and regenerated fixtures, using the same path as CI, also passes 22,823 checks
and gives identical allocation counters. The unchanged-source measurement
passes 23,623 checks with only the three candidate allocation-budget
assertions per clip disabled. Its larger count comes from checking every
additional allocation record, not from additional behavior coverage.

The corpus verifies:

* Full decoded clips against FFmpeg, exact expected output length, actual
  provider bounds and MP3 compressed-input consumption. MP3/Vorbis peak
  disagreement is one signed-16 sample unit (limit two). Fixed-point Opus
  differs from FFmpeg's floating-point decoder: observed peak errors are
  4/44/43 units for mono/stereo/long packets and 135 for six channels; limits
  are 64 and 192 respectively, with mean absolute error at most two units
  across each complete clip. Six-channel output is compared in the actual
  Vorbis channel order. There is no byte-equality claim against FFmpeg.
* Byte-identical complete PCM and decode-after-drain output between the
  baseline and candidate, all nine clips. This includes the current Opus
  end-padding behavior: the wrapper does not apply the final Ogg granule trim.
  FFmpeg covers all valid content; expected provider frame counts and the
  baseline comparison additionally cover that existing tail. This PR does
  not claim to fix gapless Opus end trim.
* Alternating nonzero poison over the entire provider destination before
  each correctness call, exposing unwritten/stale samples. Real reset-state
  Opus packet-loss concealment and a real six-channel mapping-255 muted
  channel overwrite poison with zeros.
* Independent retained output and a preexisting output prefix across input
  overwrite, later packets, flush, drain and decoder destruction. Restarted
  clips are compared packet by packet; nonzero seek preserves Opus preroll,
  MP3 delay and Vorbis page accounting. Multiple MP3 frames in one input are
  decoded and compared with the original complete clip.
* Real invalid MP3, Vorbis and Opus inputs, MP3's incomplete-header path,
  unchanged error/output behavior and recovery after reset. Actual split
  and unified Opus CSD parsing is exercised; the six-channel unified header
  preserves the independently encoded stream's coupling and channel map.

Two failing controls qualify the test. The unchanged decoder under the
candidate gate fails the allocation-call budget. A candidate mutation that
appends full Opus scratch capacity instead of the actual returned frame
count fails the complete-output-length assertion with the real decoder.
Neither negative control substitutes a successful provider.

The host run uses ASan on provider units and ASan/UBSan on the complete wrapper
and observer. Local LeakSanitizer is disabled only because the execution
environment traces the process; the committed CI invocation retains the
default sanitizer settings. The complete production decoder cross-compiles
for Windows x86_64 and aarch64, and the x86_64 probe links with the actual
provider objects. These are compile/link checks, not local Windows execution.

## Reproduction and CI

With the three existing Android source checkouts at their lock-file tag and
FFmpeg, a fresh actual-provider build and full contract run is:

    python3 tools/test_audio_decoder_scratch.py \
      --frameworks-av-root third_party/aosp/frameworks/av-media \
      --tremolo-root third_party/aosp/external/tremolo \
      --opus-root third_party/aosp/external/libopus \
      --sanitize --output-dir out/audio-after

For an A/B run, create the baseline directory with the same arguments plus
`--source-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --measure-only`.
Copy its nine `.audt` files and `fixtures.json` into the candidate directory,
then run the candidate with `--reuse-fixtures --compare-results out/audio-before`.
`--provider-library` may reuse the unchanged provider archive from the first
run. The driver requires the provider checkouts' exact pinned revisions and
validates fixture hashes. Run the baseline executable with `gate` instead
of `measure` to reproduce the failing allocation control.

The normal framework-assets job runs this suite and packages its encoded
fixtures and independent PCM references. The existing unconditional Windows
media shard runs the same production/provider probe against those fixtures;
a missing executable, failed assertion or absent success marker fails that
gate. All existing playback/application media tests remain present. Full
fork CI and actual Windows runtime results must be reported separately from
the local measurements above.

The CI change also replaces the inherited inaccessible reusable-framework
commit `06d175a84affff8914c00151269b0aa2f7f4032a` with main-reachable
`6afcb6e671fc4f2d08a4a98220f9d9a906a327bf`. Their `framework.yml` bytes are
identical (Git blob `834692396e307a6cd9251c527b7a91ab402d13ac`, SHA-256
`8b305bfd49e84b7245fdf369cfdaa13730a916d395f26a71b8f5b4d6d96a803c`).
The recipe, hosted-runner selection, source-identity checks and gate coverage
are unchanged.
