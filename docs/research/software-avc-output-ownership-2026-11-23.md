# Software AVC decoded output ownership — 23 November 2026

This is a bounded M08/T12 component of [#56](https://github.com/gesher-runtime/gesher/issues/56).
It changes the software H.264 fallback's intermediate frame delivery. Codec
selection, hardware claims, decoded pixels, crop geometry, output ordering,
consumer ownership, failure behavior and EOS remain unchanged. The associated
timestamp repair restores the actual retried access unit's timestamp after a
resolution reset. No broad roadmap row is declared complete.

## Ownership contract and implementation

The decoder creates pinned Android 16 r4 libavc with `u4_share_disp_buf=0` and
`IV_YUV_420P`. Its output buffers belong to the caller. In
[`ih264d_get_next_display_field`](https://android.googlesource.com/platform/external/libavc/+/android-16.0.0_r4/decoder/ih264d_utils.c),
the nonshared path reports the supplied application output pointers. The
[`ih264d_video_decode`](https://android.googlesource.com/platform/external/libavc/+/android-16.0.0_r4/decoder/ih264d_api.c)
path completes format conversion and releases the internal display picture
before returning, including during flush. The application output allocation is
separate from the provider's reference-picture storage.

Previously, `AvcDecoder::Emit` allocated a second packed frame, then copied every
decoded row out of its own output scratch. When the returned pointers, strides,
plane boundaries and total allocation size exactly match packed I420, it now
swaps that allocation into `VideoFrame::data`. The decoder retains no reference
to the delivered allocation. Its next call allocates different output storage
while the consumer retains the frame.

All other layouts retain the original row-copy path. This includes the normal
1920×1080 case, whose provider scratch has 1088 allocated rows, and cropped
318×238 output with a 384-byte luma stride. A provider reporting different
output pointers also takes the copy path. Frame dimensions, crop bounds,
strides and owned contiguous Y/U/V representation do not change.

## Measured copy volume and simultaneous storage

The actual production wrapper and `video_decoder.cc` helpers run against real
libavc commit `d380d04a78c6c08702e36aca86d0fd4baee390f9`, from
`android-16.0.0_r4`. FFmpeg 7.1.5 generated the inputs and independently decoded
every expected pixel. Six identical access units warm each workload. Each
measured call releases the previous delivered frames before decoding again.

| Output / measured frames | Additional row-copy bytes, before → after | Peak live large C++ frame bytes, before → after | Large allocations, before → after |
| --- | ---: | ---: | ---: |
| 384×256 / 6 | 884,736 → 0 | 294,912 → 147,456 | 6 → 6 |
| Cropped 318×238 / 6 | 681,156 → 681,156 | 251,766 → 251,766 | 6 → 6 |
| 1280×720 / 18 | 24,883,200 → 0 | 2,764,800 → 1,382,400 | 18 → 18 |
| 1920×1080 / 6 | 18,662,400 → 18,662,400 | 6,243,840 → 6,243,840 | 6 → 6 |
| 3840×2160 / 2 | 24,883,200 → 0 | 24,883,200 → 12,441,600 | 2 → 2 |

This removes one complete frame copy for exact packed output, and halves the
wrapper's simultaneous large frame storage in that steady delivery workload.
At 30 outputs/s, the avoided copy volume is 41,472,000 bytes/s for 720p or
373,248,000 bytes/s for 4K. Those rates are arithmetic from measured bytes/frame,
not measured playback speedups. The provider's own decoding/format-conversion
copies, C allocations, reference pictures, process RSS and downstream output
copies are outside these counters. Allocation count per output is unchanged.
Retaining many application frames naturally retains their storage; no bound on
application-owned frames is claimed. Drain's final non-output call can allocate
scratch again, so the table is not a whole-session peak-memory claim.

## Timestamp regression discovered by the real provider

The untouched upstream source at `df3aff782e2d411aad2d07ca39f0b21ee2ddbfe2`
fails the new real resolution-change test: the first 720p output after a
384×256 stream has timestamp `366670`, instead of its actual input's `10000007`.
The old `IVD_RES_CHANGED` path clears `times_` in `Reset()` and retries the same
access unit without restoring its marker.

A separate commit restores only `times_[marker] = time_us` after that reset.
Old reordered pictures are flushed first, and their old map is still cleared.
No timestamp is synthesized from display position or assumed monotonicity.
Both increasing and decreasing resolution now pass the independent per-access-
unit timestamp oracle, including retained old output and B-frame reordering.

To isolate copy costs, the performance reference uses that timestamp repair
with the original copying `Emit`. Source SHA256 identities:

- Original upstream decoder: `3dfc9f53cb27a2af2ceb2f13ed3e9a0e27b6519a11eed3bab4e8882b4f90d51c`.
- Timestamp-repaired copying reference: `c88f8a9ad860bf5b556ee205b1e5c6553031441ad6ad1f9afdc3eab1ebd54c37`.
- Ownership-transfer candidate: `cccbf8a2e422fe78ac6e823ff7410cd5c206297c4426432b0c99ca6c65697964`.

## Regression evidence and limits

The candidate passes 8,570 checks; the copying reference passes 8,564 with only
the six efficiency-budget assertions disabled. All 68 complete primary frame
buffers are byte-identical to independently decoded FFmpeg output on both
sides. The probe additionally checks real B frames, delayed drain output,
retained frames after more decoding and decoder destruction, explicit flush,
drain/reuse, both resolution-change directions and fatal-error teardown.

The external-pointer control forwards the real decoded pixels through separate
test-owned storage, poisons the original scratch, and poisons external storage
after `Decode` returns. Removing the pointer-identity checks fails the pixel
oracle. The original copying implementation fails the copy-byte budget. The
unmodified upstream source also fails the timestamp regression described above.
These controls are isolated from ordinary runtime behavior.

Only the Windows aligned allocator and the null `Format` metadata boundary are
adapted for the host. Unused backend-selection/metadata-write references abort
if invoked by the standalone probe; no alternative codec is fabricated. Real
`VideoFrame::Allocate`, parameter-set helpers, the complete AVC wrapper and the
actual provider are compiled. ASan covers provider C code; ASan/UBSan cover the
wrapper and probe. Local commands use `ASAN_OPTIONS=detect_leaks=0` because the
executor tracer prevents LeakSanitizer inspection; committed CI retains normal
sanitizer settings. No local leak-detection pass is claimed.

The production decoder compiles for Windows x64 and ARM64 with the CI-pinned
llvm-mingw, and the complete x64 provider/probe links. Linux CI generates real
H.264/FFmpeg fixtures and packages only their approximately 1 MiB input/oracle
files; the Windows media track runs the same complete probe on those fixtures.
Actual Windows/ART execution remains a full-CI requirement. No hardware decoder,
presentation timing or end-to-end power claim is made.

## Reproduction

```sh
python3 tools/test_soft_avc_output.py \
  --libavc-root third_party/aosp/external/libavc --sanitize \
  --output-dir out/media-avc-output-fixtures
# Reference: use the timestamp-only parent of the ownership-transfer commit.
python3 tools/test_soft_avc_output.py \
  --libavc-root third_party/aosp/external/libavc --sanitize \
  --source-ref <timestamp-only-commit> --measure-only --output-dir out/avc-before
# Without --measure-only, the copying reference must fail its copy budget.
```

`result.json` records source/provider identities, fixture hashes and metrics.
`fixtures.json` records the independent decoder version and real B-frame types.
The additional reusable-framework SHA repair points to the identical published
recipe already reviewed for #336; runner routes and acceptance gates are unchanged.

## Full-CI follow-up: collect after a completed camera frame

The full fork run on published head
`55fe2e2778f3e0ea1576117569bb99b90447d1a7`
([45131006119](https://github.com/yom-chadash/gesher/actions/runs/45131006119))
ran the new AVC ownership probe successfully in the actual Windows 2022 media
shard. That shard nevertheless failed its existing recordapp camera assertion:
the first Media Foundation `ReadSample` took 1,017,877 microseconds, leaving
only 28 frames in a two-second collection window that began before the first
capture. All 28 delivered frames were accepted, encoded and muxed with zero
queue drops or encode failures. Their decoded order and picture were correct,
but the 0.9-second file failed the unchanged duration and 30-frame minimum.
The later device-process invocation delivered 62 callbacks and passed. These
observations identify the missing fixture synchronization; they do not establish
a decoder regression or excuse the failed assertion.

The fixture now waits at most ten seconds for an actual
[`onCaptureCompleted`](https://developer.android.com/reference/android/hardware/camera2/CameraCaptureSession.CaptureCallback#onCaptureCompleted(android.hardware.camera2.CameraCaptureSession,%20android.hardware.camera2.CaptureRequest,%20android.hardware.camera2.TotalCaptureResult))
from its fresh recording session and request. Both the callback request and
result request must be that exact request; the first result's
[`getSequenceId()`](https://developer.android.com/reference/android/hardware/camera2/CaptureResult#getSequenceId())
must match the ID returned by `setRepeatingRequest`. A callback arriving before
submission returns is retained. A foreign or stale callback cannot begin the
window. Missing or mismatched completion causes explicit failure after recorder
release and camera close, with no recording data synthesized.

Only then does the original two-second collection run. The duration, minimum
30 decoded frames, order, actual test-picture pixels, capture/loss diagnostics,
and close-before-thread-quit checks remain. The Windows assertion also requires
the new owned-completion readiness line. Existing open/configure/close waits
and the overall Windows process timeout are unchanged. No production runtime,
decoder/provider or capture implementation changes in this follow-up.

`tools/test_recorder_camera_ready.py` extracts the actual callback and readiness
expression into an explicit host boundary. Opaque Camera2 identities surround
the real JDK latch and atomics; no camera frame or successful recording is
simulated. It passes 15 checks including a real ten-second no-completion timeout,
callback-before-return, asynchronous wakeup, repeated completion and sequence
mismatch. Four mutants independently removing the session, callback-request,
result-request and sequence guard each fail their corresponding assertion.
The old unsynchronized fixture fails the collection-order guard. The complete
recordapp Java sources also compile against the cached Android framework and
core modules using the APK build's compiler options. Actual Windows recording
and full-CI success at the repaired head remain pending.

The same old-head run's package job lost its hosted runner during
`Install, run Hello, uninstall`; earlier installer build/upload steps passed.
GitHub's observed annotation reports loss of runner communication. Its API log
blob is unavailable. This is the observed failure category, not a diagnosis of
an external or product cause; the new full run must exercise it again.

The branch composes published main
`5aa6b39027e9c38de2781ff9cda066418d25b1ba`. Its reusable-framework recipe has
blob `6dc4cca565aaf15fdf518eee825b0e44765b275f`, exactly equal to the unreachable
`fe42fc3d4854141df94cd1e05cfbd10c4a686315` reference inherited from main. The
caller pins the reachable main commit without changing the new recipe or gates.
