# MediaCodec Surface output without duplicate CPU frames

This is the output-slot part of **MEDIA-04** in [#178](https://github.com/gesher-runtime/gesher/issues/178).
It removes a full I420 allocation and copy that Surface consumers cannot access.
It does not complete MEDIA-04 or establish an application-level performance gain.

## Production change and compatibility

`Codec::EmitFrame` previously retained the decoded `VideoFrame` and also copied
its visible pixels into `Slot::mem`, including when a window or ImageReader
Surface was configured. Rendering already uses the retained `VideoFrame`.

[MediaCodec's output-Surface contract](https://developer.android.com/reference/android/media/MediaCodec)
makes the CPU output buffer and Image inaccessible. The implementation already
returned null for those accessors. Surface output now retains the decoded frame
without packing a second copy; a reusable slot also releases any old CPU backing
allocation. CPU output continues to use the same cropped, tightly packed I420
conversion, including odd dimensions and padded decoder planes.

Output index, size, offset, PTS, flags, asynchronous callback publication and
the eight-slot admission mechanism are unchanged. A held output retains the
original frame. Surface replacement and release still use the existing owned
window/reader references. If the current `SetSurface(null)` path removes the
consumer while a frame is held, its first CPU accessor materializes that exact
frame once; a stale CPU buffer from an earlier frame cannot be exposed.
The JNI target is built without C++ exceptions, so this lazy path uses a
separate `nothrow` allocation and reports Java `OutOfMemoryError` on failure.
Failure preserves the held frame and metadata, exposes no partial buffer and
allows a later accessor to retry. Reusing the slot releases deferred CPU data.

There is no new queue or cache. Decoder frame storage and all downstream
conversions remain. In particular, ImageReader's I420-to-NV12-to-Java transport
and GPU readback are separate remaining work. The existing EOS sentinel may
still allocate 16 bytes; it does not duplicate a decoded frame.

## Reproducible host evidence

Baseline: `8b316aebf09ab46d5407e955bf86ed748a1e739f`.
The exact production source SHA-256 values and all raw observations are in
[`codec-surface-output-2026-11-22.json`](codec-surface-output-2026-11-22.json).

The fixture compiles the production `Slot`/copy helper, `EmitFrame`, output
getters, `Publish`, `FreeOutput`, EOS and surface-selection methods. It supplies
controlled decoder frames, JNI objects, format publication and native windows;
it is not a Windows/ART or device run. `memcpy` is instrumented to count payload
bytes without changing its result. Retained capacity is the sum of the eight
actual output-slot backing allocations, not process RSS, private commit or
decoder memory.

The run used x86-64 Linux, GCC 14.2.0, C++20, `-O2` and `-fno-exceptions`. Each of the nine
configurations warms eight occupied slots, then publishes 120 frames in batches
of eight. Both window and ImageReader modes produced the following counts:

| Resolution | Copied bytes per frame, before → after | Eight-slot duplicate capacity, before → after | Copied bytes over 120 frames, before → after |
| --- | ---: | ---: | ---: |
| 1280 × 720 | 1,382,400 → 0 | 11,059,200 → 0 | 165,888,000 → 0 |
| 1920 × 1080 | 3,110,400 → 0 | 24,883,200 → 0 | 373,248,000 → 0 |
| 3840 × 2160 | 12,441,600 → 0 | 99,532,800 → 0 | 1,492,992,000 → 0 |

The CPU-output control retained identical copy counts and slot capacities at
all three resolutions. Every tested pixel matched an independent coordinate
pattern, including odd crop origins, chroma geometry and padding. The timing
fields in the raw file are single host observations for each 120-frame loop;
they exclude decoding and actual rendering and carry no confidence interval.
No whole-playback CPU percentage or latency ratio is inferred from them.

Reproduce the A/B result:

```sh
python3 tools/test_codec_output_storage.py \
  --compare-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f \
  --json /tmp/codec-output-ab.json
```

Baseline passed **954 checks**; candidate passed **999**, including 45 checks
for the new lazy-allocation failure/retry path. Running the current savings
assertions against the baseline alone is an intentionally failing negative
control:

```sh
python3 tools/test_codec_output_storage.py \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f
```

## Regression validation and limits

The candidate passed the 999-check fixture under ASan/UBSan, including CPU and
Surface getters, legacy buffer arrays, native-window/ImageReader switches,
lazy CPU access after detach, asynchronous metadata, EOS and eight held outputs.
A fault at the actual `nothrow` allocation reaches Java `OutOfMemoryError` for
the ByteBuffer, Image and legacy-array accessors; recovery produces exact pixels
once without losing the frame or retaining failed storage. A ninth producer
blocks in the real `FreeOutput`; a flush request cancels that
admission without replacing or releasing the eight client-held frames.

Existing exact-method fixtures also passed under ASan/UBSan: 25 Surface
timestamp checks, 194 codec window ownership checks, 15 receiver checks and
97 codec wait/lifecycle checks. Local LeakSanitizer could not execute under
this container's ptrace environment, so those local runs used
`ASAN_OPTIONS=detect_leaks=0`; address and undefined-behavior checking stayed
enabled. The CI command does not disable LeakSanitizer.

`videoapp` now treats any CPU-buffer/Image exposure for Surface decoding as a
failure, including the legacy output array. Its existing Windows test still
requires 8 TextureView frames and 38 SurfaceView frames, their expected red/blue
pixels, normal CPU ByteBuffer/Image decoding, callback operation and flush/seek.
These Windows/ART runs were not executed in this local Linux session. Relevant
Windows CI, device playback, whole-process memory/CPU and long-duration release
qualification remain required; the host fixture does not substitute for them.
