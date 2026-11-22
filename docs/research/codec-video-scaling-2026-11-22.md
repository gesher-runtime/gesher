# MediaCodec display scaling without intermediate frame buffers

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), M05 video-scaling
leaf. Initial base: `bf7c6e2f1adac284bd00971734f7b50c63d72a94`.

## Android contract and bounded scope

`MediaCodec.setVideoScalingMode` was an unconditional native no-op, including
invalid modes and a released receiver. This change implements scale-to-fit (1)
and scale-to-fit-with-cropping (2) for decoded video displayed through Gesher's
QueueSurface-backed SurfaceView and TextureView, including surfaces parceled to
a producer in another process.

The [Android MediaCodec API](https://developer.android.com/reference/android/media/MediaCodec#setVideoScalingMode(int))
defines fit, aspect-preserving crop, the default fit mode, invalid-mode
`IllegalArgumentException` and released-state `IllegalStateException`.
The pinned [Android 16 r4 JNI implementation](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/media/jni/android_media_MediaCodec.cpp)
checks the receiver before validating the mode. Its `JMediaCodec::setVideoScalingMode`
has no effect before an output Surface exists.

The pinned [GLConsumer implementation](https://android.googlesource.com/platform/frameworks/native/+/android-16.0.0_r4/libs/gui/GLConsumer.cpp)
separates `getCurrentCrop` from its raw texture transform. Its `scaleDownCrop`
uses integer center cropping, with an odd discarded pixel assigned to the
right/bottom. Its raw `computeCurrentTransformMatrixLocked` does not add video
scaling to the texture transform. The [hwui updater](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/DeferredLayerUpdater.cpp)
passes the consumer crop separately to the display layer.

This implements that display contract while preserving the application's actual
decoded input, timestamps, Surface ownership and pixel storage. It adds no test
inputs to the runtime and reports no additional decoder or hardware capability.
The synthetic color bars exist only in the explicitly invoked surfaceapp test
Activity and its private test service.

This is one M05 leaf, not closure of every codec or graphics requirement. It does
not add scaling to the direct Win32Surface backend, general native-window
transforms, rotation handling, sample-aspect-ratio support, GPU decode or a
compositor. Existing ImageReader and raw SurfaceTexture GL behavior is preserved.

## Implementation and ownership

The codec stores its setting under the existing codec mutex. Configure and stop
retire the session setting to the default. A valid call with no output Surface
has no effect. The JNI receiver check still runs before mode validation; unknown
modes fail without changing state. A concurrently released valid receiver is
also rejected by the locked setter.

`ReleaseOutput` captures the mode under the same lock as the decoded frame,
timestamp and acquired native-window reference. A later setter or
`setOutputSurface` cannot change that admitted delivery. `RenderFrame` preserves
the full decoded image and publishes the mode in the immutable `SurfaceFrame`.
Ordinary Canvas/native-window publications keep the default fit mode; recycling
always initializes the next frame's metadata explicitly.

SurfaceView and TextureView select the source rectangle during their existing
draw. Mode 1 keeps the whole source. Mode 2 selects the centered integer crop
and uses Skia's strict source constraint so linear sampling cannot bleed outside
the crop. Layer transforms, alpha, blend mode, color filter and rounded clipping
retain their existing paths. The image cache and decoded pixel buffer remain
shared by their existing owners. A view resize changes only its source rectangle.

`TextureView.getBitmap(w, h)` computes the displayed crop using the view/layer
dimensions, then draws it into the requested Bitmap dimensions. A differently
shaped bitmap therefore captures the same displayed content.

The private Surface forwarding protocol appends an optional two-int tail
`version=1, mode` only for non-default frames. Old producer payloads have no tail
and mean fit. The receiver requires an exact eight-byte extension, known version,
successful reads and mode 1 or 2 **before mapping** the producer's memfd. Invalid
extensions return `BAD_VALUE`. Existing memfd reuse and FD lifetimes are unchanged.
The mode travels with each publication, so returning to a default frame cannot
reuse an earlier crop setting. Mixed old receiver/new crop-producer behavior is
not a supported version-negotiation claim.

## Resource cost

The scaling operation adds one integer of metadata to the frame object and
constant-size crop arithmetic. It introduces no frame-sized crop/resample
allocation, secondary decode or extra pixel pass. The normal render conversion,
QueueSurface publication, Skia image ownership and Surface forwarding payload
remain as before. Forwarded fit frames have zero additional protocol bytes;
forwarded crop frames add eight bytes.

The focused fixture verifies that both actual draw paths receive the original
immutable pixel pointer, and that resize changes the visible crop while retaining
the same frame number and pixel storage. Its Skia boundary deliberately allocates
its own small readback image for assertions; that is not runtime allocation
measurement. The existing production frame-recycling suite still reports zero
steady-state payload allocations at 64x64, 720p, 1080p and 4K. Those measurements
exercise ordinary frame publication and establish regression coverage, not an
application-wide speedup or a before/after improvement over the old no-op.

## Production-backed host validation

`tools/test_video_scaling_contracts.py` compiles the complete production
QueueSurface header/implementation and RenderFrame, the actual image cache,
TextureLayer/TextureLayerDrawable/ViewSurfaceDrawable, Bitmap copy method, and
the actual optional Parcel reader/writer. Platform native-window dispatch,
I420 conversion, Skia rasterization, JNI handles and Parcel byte storage are
explicit boundaries. Source hashes are printed for every production input.

The focused run passes **42,157 assertions** with AddressSanitizer and
UndefinedBehaviorSanitizer, including 10,000 comparisons with the exact Android
16 r4 `GLConsumer::scaleDownCrop` method. Without that optional primary source,
the self-contained CI run contains 32,165 assertions. Cases cover:

- Fit and crop in both actual display consumers and Bitmap readback, including a
  Bitmap whose aspect differs from the view; unchanged paint/transform/clip state.
- Odd and extreme dimensions, bounds, integer centering, empty/invalid sources,
  unsized (0x0) targets keeping the whole source in both modes,
  overflow-safe arithmetic, and the official algorithm comparison.
- Actual RenderFrame publication, full raw pixels, timestamps, retained owners,
  resize without a new frame, surface replacement, live return to fit, abandon
  and its error, and concurrent immutable mode/pixel/timestamp publication.
- Legacy/default Parcel payloads, mode changes, exact tail size, unknown version,
  invalid mode, trailing/truncated bytes and either integer read failing.

The exact primary-source file has SHA256
`25cdc494a33f89c8a40aefb5b022b54435b90e277bfbba8c19789f9a8f2b5805`.
The comparison rejects a different reference file. Only the temporary test
translation unit receives the extracted AOSP method; the production helper is
small independent arithmetic over the documented crop contract.

The existing exact-production codec ownership fixture now includes the actual
setter and JNI wrapper: **222 assertions**, including mode errors, no-Surface
behavior, concurrent mode/Surface replacement during an admitted release, reset
and released-state precedence. Its native-window dispatch/receiver lookup are
host boundaries; the APK separately exercises real JNI receivers.

Additional passes: 25 timestamp, 15 receiver, 97 codec-wait, 158 surface-geometry,
38 surface-release, 48 image-cache and 351,684 frame-recycling assertions.
Codec ownership, timestamps, receiver/wait, focused scaling and frame recycling
were run with ASan/UBSan. Geometry/cache also passed their ordinary host runs.
LeakSanitizer was disabled locally because this executor uses ptrace; the checked-in
CI commands do not disable it.

### Reproduction

```sh
python3 tools/test_video_scaling_contracts.py --sanitize
python3 tools/test_video_scaling_contracts.py --sanitize \
  --android-reference /path/to/android-16.0.0_r4/GLConsumer.cpp
python3 tools/test_media_surface_contracts.py --sanitize
python3 tools/test_codec_lifecycle_contracts.py --sanitize
python3 tools/test_surface_frame_recycling.py --sanitize
python3 tools/test_surface_geometry_contracts.py
python3 tools/test_surface_image_cache.py
```

Negative controls compile real production sources from the original base while
keeping the new assertions:

```sh
python3 tools/test_video_scaling_contracts.py \
  --source-ref bf7c6e2f1adac284bd00971734f7b50c63d72a94
python3 tools/test_media_surface_contracts.py \
  --source-ref bf7c6e2f1adac284bd00971734f7b50c63d72a94
```

The old display implementation fails nine focused checks: crop metadata, actual
consumer source rectangles, visible stripe, strict sampling, Bitmap crop, Surface
replacement, invalid mode admission and immutable concurrent mode publication.
The old setter fails eleven error/state/capture checks. Both compile successfully;
these are behavioral failures rather than fixture compilation failures.

## Real Windows/ART acceptance, wired but not locally executed

The surfaceapp fixture adds `CodecScalingActivity` and a private `:scaling`
service. Its test producer encodes a 192x64 I420 red/green/blue frame through the
real AVC encoder, then sends that compressed key frame through two real decoders.
Each render is a complete stream: the key frame, then END_OF_STREAM; the
producer renders the drained frame and flushes, which keeps the scaling mode.
No runtime backend or capability is replaced by this fixture.

`tests/windows/tracks/52-surfaces.ps1` invokes this Activity directly, then through
the managed device with its decoder service in a different process. The latter
asserts different producer/consumer process IDs and sends the real Surface objects
over Binder. Both modes require actual window pixel readback for all six phases:

1. Default fit after a valid pre-configure setter call without a Surface.
2. Live center crop in SurfaceView and TextureView.
3. Resizing the display to the source aspect without sending another decoded frame.
4. Replacing the TextureView output Surface while retaining the live mode.
5. Switching back to fit, including the absent/default IPC tail after crop.
6. Resetting/reconfiguring the codec and receiving default fit.

Every phase also checks a real `TextureView.getBitmap(192, 64)` against the
displayed content. Real JNI invalid-mode and released-state errors are required.
The window harness rejects missing shots, failed pixels, missing phase/completion
markers, identical supposedly separate process IDs, cleanup errors and a nonzero
process exit. It reads only the new app-log suffix so a prior Activity's HWND or
completion marker cannot satisfy the second run.

The complete surfaceapp Java sources compiled with the real Android framework
classes and libcore module, then D8 produced DEX. This is compilation evidence;
the new Windows-native build, ART, decoder/Skia execution and Binder-window runs
remain a mandatory CI acceptance step, not a claimed local pass.

## Composition with merged ImageReader delivery

After the implementation commit, current upstream
`324dc7e41ea13ceedc3d52db3c8f7c2f3590e452` was merged without conflicts. This
includes the first-wave native ImageReader delivery (#273), HTTP/2 (#275) and
memmem (#274) changes; provider indexing (#272) was already in the initial base.
Only the codec JNI file and CI workflow intersected this patch. The 21 disjoint
files stayed byte-identical, and the JNI added/removed lines matched the original
scaling patch exactly after composition.

On the merged tree, ASan/UBSan passed 200,977 native reader-delivery assertions,
222 codec ownership/scaling assertions, 25 timestamp assertions and the 32,157
self-contained scaling assertions. The reader fixture still reports zero Java
intermediate array and copy bytes at 720p, 1080p and 4K. Surfaceapp's complete Java
and DEX build also passed again. These checks verify the shared release path and
test wiring while retaining the Windows/ART limitation stated above.
