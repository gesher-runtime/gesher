# Exact scaled ImageReader planes: source and complete-delivery evidence

This is a measured component improvement for issue #56 / T12, with media and
ImageReader regression coverage. It does not close the broader M03/T09/C06 or
device/application acceptance rows. The [raw JSON](reader-plane-scale-2026-11-23.json)
retains every pilot and final process, source/build identity, original sanitizer
environment failure, compiled negative and independent review receipt.

## Change and compatibility boundary

The existing `CopyReaderPlane` computes `floor(x * width / target_width)` for
every output byte. The candidate computes the quotient and remainder once per
plane and walks exactly those coordinates with integer addition and one carry.
After x steps its source coordinate is `floor(x * width / target_width)` and its
error is `(x * width) % target_width`. The remainder is smaller than the target
width, so at most one carry is needed per step. The error accumulator is 64-bit;
the source coordinate stays within the positive source width and the final
post-read update reaches at most that width. Vertical mapping is unchanged.

The production diff adds 22 lines in this function. Matching-size bulk and padded
row copies remain before the new guarded path. The old nonpositive-input fallback
remains verbatim; public frame validation still occurs in the original caller.
No coordinate map, retained state, cache, allocation, thread, fabricated camera
capability or replacement input is added. Native callback, FIFO, publication,
acquisition, status and close rules remain in the existing production methods.

The baseline is `3b2eaca95b5ddd9cee3c3db4e430460e512eb355`.
`camera_frames.cc` is SHA-256
`5cf8388aac7fe4cb5bc0977213e07ce03610a80947c0e11bcd5206cc638d41c4`
before and `dc3f1f40bfc55a061bfda54f413fcfc6c99513e6a7d2eebbf73e81bd372b4638`
after. This production source and the measured C++ fixture did not change between
the pilot and final campaigns.

[Android's ImageReader documentation](https://developer.android.com/reference/android/media/ImageReader#getWidth())
allows a Surface producer to override the reader's default dimensions; the actual
size then comes from the acquired Image. Consequently the four requested-size
assertions in the new real codec/ART fixture explicitly preserve Gesher's existing
floor-resize behavior. They are **not Android-reference resizing parity**. The
host PRIVATE three-plane case likewise checks existing internal storage, not a
claim that public Android PRIVATE images expose readable planes.

## Measurement boundary

The final campaign ran **16:13:24.593378–16:13:37.416567 UTC on 23 November 2026**,
after all three other agents explicitly held compilation and CPU-heavy replay.
Six ABBA blocks rotate eleven cases; every invocation is a fresh process with a
first single-frame and a steady multi-frame phase: **264 processes / 528 records**,
twelve observations per variant/case/phase. The separate original one-block pilot
contains 44 processes / 88 records. No measurement process or adverse row was removed.

Each timed operation creates its real native reader owner, calls the actual
`DeliverToReader` → `QueueImageReaderVideoFrame` → `QueueVideoFrame` →
`CopyReaderPlane` / `EnqueueImage` path, acquires the actual queued image, compares
all output bytes and metadata, consumes a full-output checksum, releases every
image, and closes/destroys the owner. Already decoded input and the independent
old-coordinate oracle are prepared before timing. The JNI and ReaderSurface
adapters are explicit. Decoding, ART, GPU, IPC, a physical camera and whole-process
startup are outside this boundary. The first phase is the first delivery on a
fresh owner after fixture preparation, not an application cold start.

Ordinary timing uses GCC 14.2.0 `-O2`, strict warnings, normal allocators and no
sanitizer or replacement-allocation observer. Process CPU uses
`CLOCK_PROCESS_CPUTIME_ID`; wall time uses `CLOCK_MONOTONIC`. The benchmark retains
both raw process output and parsed rows and checks every output/checksum/count
signature before summarizing. The independent reviewer recomputed **3,441 checks**
over all 308 processes / 616 records, including ABBA order, every build/source/binary
identity and each median, minimum, maximum, nearest-rank p95 and percentage.

### Final medians

Times are microseconds per frame, including the complete boundary above.
Negative percentages mean reductions. Cropped/padded cases retain actual plane
strides and odd crop offsets; only visible dimensions are shown.

| Workload | Steady CPU before | Steady CPU after | CPU change | Wall change | First CPU before → after (change) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8×8 → 3×5, cropped/padded | 0.225533 | 0.169935 | -24.65% | -24.66% | 11.2990 → 11.2815 (-0.15%) |
| 1920×1080 → 960×540 | 2693.772094 | 651.347250 | -75.82% | -75.82% | 3031.7255 → 961.1630 (-68.30%) |
| 1920×1080 → 1280×720, cropped/padded | 4661.595094 | 1238.891687 | -73.42% | -73.43% | 5328.4010 → 1821.1470 (-65.82%) |
| 640×360 → 1280×720 | 4650.963625 | 1171.444844 | -74.81% | -74.82% | 4963.3470 → 1611.5340 (-67.53%) |
| 1280×720 → 1920×1080, cropped/padded | 10576.038917 | 2646.160583 | -74.98% | -75.00% | 11708.7665 → 3908.4065 (-66.62%) |
| 3840×2160 → 1920×1080 | 10531.130625 | 2754.692500 | -73.84% | -73.89% | 12248.2895 → 4445.5105 (-63.71%) |
| 1280×720 → 641×361, internal PRIVATE control | 1136.858146 | 324.587458 | -71.45% | -71.45% | 1301.3155 → 471.6265 (-63.76%) |
| 1280×720 identity | 259.986833 | 248.312688 | -4.49% | -4.29% | 819.6925 → 725.0530 (-11.55%) |
| 1280×720 identity, cropped/padded | 262.331687 | 268.865854 | +2.49% | +2.49% | 855.7980 → 874.9005 (+2.23%) |
| 640×360 → 320×180 RGBA control | 305.977771 | 307.158813 | +0.39% | +0.39% | 368.5410 → 383.2925 (+4.00%) |
| Already closed reader control | 0.009807 | 0.009842 | +0.36% | +0.29% | 7.4325 → 7.4220 (-0.14%) |

All six paired block ratios are favorable in each of the six large scaled-plane
cases. Their steady CPU reductions are 71.45–75.82%; the tiny scaled case improves
24.65%. The original per-output-byte floor division is avoided while the exact
decoded-plane bytes remain the same. This establishes a substantial improvement
for these measured component workloads, without generalizing to a whole app.

### Adverse samples and statistical limits

The unchanged padded identity control has steady CPU **+2.49% /6.5342µs** and first
CPU **+2.23% /19.1025µs**. The RGBA control has first CPU **+4.00% /14.7515µs** and
steady **+0.386% /1.1810µs**. The closed-reader steady difference is
**+0.363% /0.0356ns**; its first wall median is 1.57% worse. Tiny first wall is 2.49%
worse despite slightly lower CPU. These results stay in the tables and raw data.

The earlier small pilot was much noisier: padded identity steady +11.98%, identity
first +97.56%, tiny first +24.68%, RGBA first +8.85% and closed-reader first +88.74%.
The production source was frozen before the six-block campaign; no candidate
tuning or deletion of pilot samples explains their absence from the final medians.
These observations do not establish a scheduler/compiler cause, a universal
latency improvement or an accepted release budget. With twelve observations per
variant/case/phase, nearest-rank p95 is the maximum and is not a stable tail bound.
No allocation/RSS reduction, energy saving or native Windows speed is claimed.

## Correctness and ownership

Both exact baseline and candidate pass **397,680 ASan/UBSan assertions**. Direct
plane tests cover every positive source/target width from 1 through 128, four target
heights, padded source rows, guarded output and input immutability. Each logical
pixel is compared to the original independent 64-bit floor-coordinate formula.
Complete delivery tests cover downscaling, upscaling, noninteger ratios, odd
targets, all three planes, crop and padding, RGBA/internal PRIVATE controls,
timestamps, plane offsets/strides, input ownership and closed-reader status.

The original production-path reader fixture also retains `maxImages`, pending
FIFO, independent writable held images, producer reuse, release, callback reentry,
callback-side close, exception status and a deterministic close/publication race.
The allocation observer needed for the controlled race is present only in
correctness builds; timing builds use ordinary allocation. There is no weakened
CI assertion to accommodate the optimization.

| Separately compiled mutation | Required failing assertion |
| --- | --- |
| Start the remainder at half a target pixel | Exact floor-coordinate oracle |
| Reduce the integer source step | Exact floor-coordinate oracle |
| Delay a carry on exact equality | Exact floor-coordinate oracle |
| Replace padded source stride with width | Exact floor-coordinate oracle |
| Publish despite a reader close during conversion | Close racing conversion is rejected |

All five compile successfully and fail with exit 1 at their intended assertion.
Media independently replayed the 397,680 checks and all five mutants against the
exact reviewed camera and fixture hashes. Initial local sanitizer executions
ended in LeakSanitizer's ptrace-environment limitation; their complete failed
logs remain. The successful local ASan/UBSan runs explicitly set
`ASAN_OPTIONS=detect_leaks=0`; they are not successful LeakSanitizer results.
The CI command itself retains its normal sanitizer environment.

## Actual Java integration and platform qualification

`ImageReaderPlaneScalingTest` decodes the existing explicit camera-fixture
recording `rec_camera.mp4` through real MediaExtractor and MediaCodec APIs. The
decoded first-frame CPU Image provides the reference bytes, and its luma must
actually vary spatially. Four further real decoders render the same first frame
into 160×120, 213×161, 641×481 and 320×240 readers. All logical plane bytes, reported
strides/bounds and exact default timestamps are checked. Acquired reader Images
remain valid and unchanged while the codec is stopped/released; all are closed
before their reader. Callback delivery uses an owned HandlerThread and a latch,
with one absolute 30-second fixture budget and a bounded 2-second thread join.
Cleanup preserves the primary exception and suppresses subsequent cleanup errors.

RecordActivity invokes the fixture after its existing ImageReader cases. Mandatory
Windows track 59 requires its exact success marker in both single-process and
Gesher-device modes, while retaining all existing process exits, markers and
deadlines. The complete actual recordapp Java sources compile against existing
real framework/libcore classes and real D8 produces classes.dex (16 compiled
classes). The final wording/source hashes are in a separate fresh build receipt.
This is Java/D8 qualification, not local APK resource packaging or ART execution.

The full unchanged and candidate camera translation units compile under the
project's strict Windows x64 C++23 LLVM-MinGW flags, using the actual pinned
Android 16 r4 AOSP header closure. All 68 actual dependency paths/hashes, commands,
diagnostics and object hashes are retained. PowerShell parses the final Windows
gate. Native AArch64, actual Windows/ART execution, decoder integration outcomes
and the full aggregate remain pending the genuine full fork CI.

The exact published #382 and #389 camera trees were reconciled with this source:
all six three-way composition orders are conflict-free and produce the same
camera SHA-256 `2eedfcc75a7806aab8401b196f5fe761541647bf0c87e68a8f9d3846ceffd452`.
That is a focused source-composition proof, not a claim of an automatic whole-PR
merge or a combined Windows run. Each correctness command must remain when those
PRs are integrated. The current CI diff adds only
`python3 tools/test_reader_plane_scaling.py --sanitize --negative-controls`.

Scope review against AGENTS.md and CONTRIBUTING.md: compliant for this narrow
transformation of actual supplied frames. Explicit fixtures remain test-only;
Android-reference, actual platform, application and release-budget evidence
remain required for their broader issue rows.

## Reproduction

```sh
python3 tools/test_reader_plane_scaling.py --source-ref 3b2eaca95b5ddd9cee3c3db4e430460e512eb355 --sanitize
python3 tools/test_reader_plane_scaling.py --sanitize --negative-controls
python3 tools/test_reader_plane_scaling.py --source-ref 3b2eaca95b5ddd9cee3c3db4e430460e512eb355 --timing --build-only --build-output /tmp/reader-scale-before
python3 tools/test_reader_plane_scaling.py --timing --build-only --build-output /tmp/reader-scale-after
python3 tools/benchmark_reader_plane_scaling.py --baseline-build /tmp/reader-scale-before --candidate-build /tmp/reader-scale-after --blocks 6 --output /tmp/reader-scale-abba.json
```

Output paths must be new. Normal CI should retain its default sanitizer settings;
only an environment that exhibits the archived ptrace limitation needs the
explicit local LeakSanitizer qualification described above.
