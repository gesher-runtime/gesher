# MediaMetadataRetriever frame selection after decoder drain

## Observed Windows failure

The full Windows 2025 job for commit `223d4b3b6a4947bd3c1b3c679d95fc944b4d956a`
failed the existing H.264 retriever pixel contract:

- [Run 42453120565, job 128649121895](https://github.com/gesher-runtime/gesher/actions/runs/42453120565/job/128649121895), raw log lines 2138 and 2179.
- `getFrameAtTime(1250000, OPTION_CLOSEST)` returned a white 320×240 frame;
  that point in the clip is blue.
- The previous-sync request at 1.6 seconds and scaled request at 0.25 seconds
  still returned the expected white and red frames.
- The same job decoded all 60 H.264 frames in order through MediaCodec, and the
  other retriever codec fixtures passed.
- The corresponding Windows 2022 job 128649121898 passed the existing retriever
  pixel contract. This is consistent with a decoder-buffering-dependent defect.

The old retriever loop handled frames emitted while decoding and frames emitted
at end of stream differently. If the decoder retained its output until drain,
the loop selected `frames.back()` without consulting the requested timestamp or
option. A tail containing blue frames followed by white frames therefore returned
the final white frame for a blue target. The controlled decoder regression
reproduces that defect deterministically. The exact Media Foundation buffering
schedule inside the failed CI invocation was not traced.

Two adjacent timestamp defects were also visible in the same loop. It discarded
the extractor's closest-sample target metadata, and its one-millisecond early
tolerance both selected an earlier frame and overflowed for very large signed
timestamps. No existing Windows pixel expectation is relaxed by this change.

## Android 16 contract and repair

The reference is the repository's pinned `android-16.0.0_r4` framework:

1. [VideoFrameDecoder in FrameDecoder.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/libstagefright/FrameDecoder.cpp)
   passes the requested seek mode to the extractor. For `SEEK_CLOSEST`, it retains
   the first input sample's target timestamp, then accepts the first decoded
   presentation timestamp at or beyond that target. An absent target accepts the
   first output. This output rule applies when delayed frames emerge at EOS too.
2. [MPEG4Extractor.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/module/extractors/mp4/MPEG4Extractor.cpp)
   identifies the closest presentation sample, starts decoding at an earlier sync
   sample, and supplies the desired presentation time separately. The timestamp
   of the first encoded sample is therefore not necessarily the desired output.
3. [SampleTable.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/module/extractors/mp4/SampleTable.cpp)
   performs the closest-time lookup and its tie/boundary handling. Gesher leaves
   this choice with the actual AOSP extractor instead of approximating it in JNI.
4. [NdkMediaFormat.cpp](https://android.googlesource.com/platform/frameworks/av/+/android-16.0.0_r4/media/ndk/NdkMediaFormat.cpp)
   defines `AMEDIAFORMAT_KEY_TARGET_TIME` as `target-time`.

`Sample` now carries the optional target timestamp. The actual `AospTrackReader`
reads it from sample metadata and resets it when the next sample has none, so a
previous seek's target cannot leak into a later read. The value is internal to the
compiled media adapter; this does not change a public Android ABI.

`DecodeRetrieverFrame` now requests `SEEK_CLOSEST` for `OPTION_CLOSEST`, retains
only the initial target, and applies one presentation-time predicate to both
ordinary output and drained output. Sync options preserve their extractor seek
mode and select the first decoded frame from that seek. No timestamp addition is
needed. A real read/decode failure remains a no-frame result, and the existing
600-input bound remains in place. Clean EOF after the 600th permitted input still
drains the decoder once; an unbounded stream does not extend the budget.

The JNI method still creates its bitmap from the selected frame's actual pixels,
including the existing dimensions, crop, scaling, and color conversion. Decoder
selection and the Media Foundation implementation are unchanged.

## Executed verification

| Check | Result and scope |
| --- | --- |
| Current production selector and reader wrapper, GCC `c++`, ASan/UBSan | **133/133 assertions pass**. |
| Exact published 7e selector and reader, same fixture | **78/133 pass, 55 fail**. Includes the drain-tail wrong-frame witness, closest target/mode, metadata propagation, sync-frame drain selection, and error/EOF separation. |
| Published 7e selector under UBSan, independent review | Signed overflow is detected in the old timestamp-plus-1000 predicate. |
| Complete `media_jni_others.cc`, exact CI LLVM-MinGW toolchain, Windows x86_64 | Compiles with C++23 and the target's strict warning options. |
| Independent implementation review | Selector, reader metadata, pinned key spelling, current sanitized fixture, and published-source negative control checked separately. |
| Existing actual Windows retriever pixel contract after this repair | **Pending the new CI run.** Prior failure and Windows 2022 pass are observations of 223d, not proof of the repaired tree. |

The host fixture copies the exact production `AospTrackReader` class and
`DecodeRetrieverFrame` function into a test translation unit. It uses the real
`Sample`, `TrackReader`, `VideoDecoder`, and `VideoFrame` declarations. Controlled
AOSP-C-boundary and decoder implementations supply encoded-sample metadata and
presentation-ordered frames. They exercise immediate output, seven-frame batches,
and output delayed entirely until drain, preserving a distinct blue/white payload.

Additional controls cover all four seek options, first-sample-only target
retention, absent/zero metadata, a one-microsecond boundary, negative and extreme
signed requests, an unreached target, empty input, failed start/read/decode,
no-output drain, balanced sample ownership, and the 600/601-input boundary.

This host test does not decode H.264, execute Media Foundation, create an Android
Bitmap, run ART, or establish the native Windows extractor ABI. The unchanged
full Windows videoapp suite supplies that integration check. The complete
extractor translation unit is also compiled by the normal media CI build; only
its exact reader wrapper was executed locally. LeakSanitizer cannot enumerate
threads in this executor, so the local sanitized run used
`ASAN_OPTIONS=detect_leaks=0`; ASan/UBSan and explicit buffer/lifecycle assertions
remained active.

Reproduce the current host check from the repository root:

```sh
python3 tools/test_retriever_frame_contracts.py --sanitize
```

For the negative control, extract the two original files into a disposable
directory and pass their paths without modifying the checkout:

```sh
git show 7e62479cb821aad8e203e6be6ec58a52e57d738b:compat/media/jni/media_jni_others.cc > "$TMPDIR/retriever-before.cc"
git show 7e62479cb821aad8e203e6be6ec58a52e57d738b:compat/media/player/extractor.cc > "$TMPDIR/reader-before.cc"
python3 tools/test_retriever_frame_contracts.py \
  --jni-source "$TMPDIR/retriever-before.cc" \
  --reader-source "$TMPDIR/reader-before.cc"
```

Set `TMPDIR` to a writable disposable directory when the environment requires
one. No source-identity predicate, supplier receipt, test timeout, pixel marker,
or existing test selection was weakened.

## Reference hashes

The following SHA256 values identify the decoded files fetched from the exact
Android 16 tag, not from a moving branch:

| File | SHA256 |
| --- | --- |
| `FrameDecoder.cpp` | `a2cef93b93024208c3aab1516bcdf840afde5f84e0d8413060d00c1c53d69df8` |
| `MPEG4Extractor.cpp` | `47e01224a4bd9c564901e4fdf84ff61123dbd2105b1a0aa0376a84c561d46d7d` |
| `SampleTable.cpp` | `f717ca15fbb05889a8fde1b30cb880075d44eb14aceceed757561ddff5d5db54` |
| `NdkMediaFormat.cpp` | `caf6099f6cda1b50724987ba8fdf83b7ed81d25027bdaa209e98416d134cf508` |

## Remaining bounds

The pre-existing 600-input limit can still prevent retrieval from a very long GOP.
The negative-time request still starts at time zero when this adapter has no
thumbnail-time metadata. Thumbnail-time selection, frame-index APIs, hardware
decode scheduling, and broader bitmap format support are outside this bounded
PTS/EOF repair. A failure to produce the requested frame is not reported as a
successful fabricated result.
