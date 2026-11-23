# ImageReader: matching-size owned RGBA snapshots

This is a measured component leaf for #56 M18/T12. `QueueRgba` previously
recomputed nearest-neighbor coordinates and assigned every packed RGBA pixel
even when the producer and reader dimensions matched. For equal, nonempty
extents the candidate copies the packed bytes into the image's existing newly
allocated vector. Differing sizes retain the original coordinate loop. The
zero-extent guard avoids calling `memcpy` with potentially null empty buffers.

The reader still owns a separate writable snapshot. This change adds no cache,
pool, retained object or borrowed producer pointer. It preserves the existing
image allocation, metadata, plane layout, queue admission, callback, acquired
image limit, locking and retirement. It does not implement the broader
BufferQueue slot/fence requirements, NDK AImageReader APIs, vsync scheduling,
PRIVATE/YUV handling or application performance budgets.

## Source and Android contract

The measured baseline is published commit
`8f53b6045e26f0bb4a482c69099d084db3e94963`. The baseline complete
`camera_frames.cc` SHA256 is
`5cf8388aac7fe4cb5bc0977213e07ce03610a80947c0e11bcd5206cc638d41c4`;
the candidate is
`afa6fcffb4ad9753d9086e01e631adeb723b074bde6edb7fd83b987c291db152`.
Both exact `QueueRgba` body hashes, all extracted source and fixture hashes,
compiler commands and executable hashes are in the
[raw report](image-reader-rgba-2026-11-23.json).

Android's [ImageReader contract](https://developer.android.com/reference/android/media/ImageReader)
describes queued images produced through its Surface, concurrent acquisitions
bounded by `maxImages`, and explicit `Image.close()` retirement. An acquired
image must keep its own contents while a later frame is produced. Access after
`ImageReader.close()` is not permitted by that public contract. The host
fixture's internal native-storage assertion after owner close checks safe
retirement of an already-held allocation; it does not extend the Java API.

Source review follows the actual packed producer chain: `QueueSurface::PostFrame`
owns width-times-height pixels, `ReaderSurface` keeps the frame alive through the
synchronous `QueueRgba` callback, and `Image::data` remains independently owned.
The change does not replace live input, fabricate device capabilities, alter
permissions or select an application-specific behavior.

## Complete component measurement

Final timing used six ABBA blocks for each of eight cases. Case order rotates
between blocks. Every A/B invocation starts a fresh executable, producing one
first-frame observation and one steady multi-frame observation. There are
**192 processes / 384 observations**, with 12 observations per variant, case and
phase. The earlier one-block pilot retains another **32 processes / 64
observations**. No observation was excluded and no production change was made
after seeing the pilot.

The final interval was **2026-11-23 13:07:09–13:07:26 UTC**. All three other
agent lanes confirmed that their compilers, tests and timing jobs were idle for
the interval. This controls work in this shared environment; it does not claim
exclusive ownership of the underlying host or stable device power states.

The timed interval contains native reader creation, every actual
`QueueRgba`/`EnqueueImage`, the actual native acquire method, comparison of every
output pixel with an independent oracle, metadata checks, each image release,
and reader close/destruction. The steady phase uses one reader for its entire
frame sequence. Two prepared producer buffers alternate. Input generation and
the independent nearest-neighbor oracle are outside timing. Every process
checks output checksum, frame count, assertion count and zero failures against
the other variant.

The maintained host fixture extracts production method bodies and explicitly
adapts JNI objects, VM attachment and callback delivery. It is **not** an ART,
Canvas, Binder, GPU, physical camera or Windows/UCRT benchmark. The full pixel
comparison is deliberately inside the boundary and contributes to both times.
The first phase starts after synthetic input/oracle preparation, so it is not
whole-process or whole-application cold start.

Timing uses ordinary C++ allocation, with the fixture's replacement allocators
and copy observers compiled out. GCC 14.2.0 uses `-O2`, C++20 and strict warnings.
CPU is measured with Linux `CLOCK_PROCESS_CPUTIME_ID`; elapsed time uses
`CLOCK_MONOTONIC`. Sanitizers and negative controls run separately. The raw
report includes host identity, CPU affinity, every command, unmodified process
stdout/stderr, timestamps, durations and all first/steady statistics.

### Final medians

Times are microseconds per delivered frame. Negative percentage is a reduction.
The last column includes a fresh native reader and its destruction for a single
frame; steady medians amortize owner setup over the declared frame count.

| Case | Steady CPU before | Steady CPU after | CPU change | Wall change | First CPU before → after (change) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1×1 | 0.078 | 0.079 | +1.78% | +1.78% | 8.175 → 9.575 (+17.13%) |
| 16×8 | 0.556 | 0.081 | −85.37% | −85.37% | 8.347 → 9.392 (+12.53%) |
| 257×129 | 140.464 | 8.174 | −94.18% | −94.18% | 196.273 → 80.459 (−59.01%) |
| 1280×720 | 4453.546 | 680.038 | −84.73% | −84.73% | 6401.456 → 2277.238 (−64.43%) |
| 1920×1080 | 9179.524 | 1589.805 | −82.68% | −82.88% | 11541.983 → 4372.178 (−62.12%) |
| 3840×2160 | 38863.228 | 9585.816 | −75.33% | −75.33% | 68220.640 → 25322.561 (−62.88%) |
| Scaled control: 257×129 → 129×67 | 34.036 | 34.747 | +2.09% | +2.06% | 49.945 → 49.401 (−1.09%) |
| Empty control: 0×5 | 0.063 | 0.062 | −2.63% | −2.65% | 7.015 → 6.641 (−5.32%) |

### Adverse results and limits

First 1×1 CPU increases **1.400 μs / 17.13%**, and first 16×8 increases
**1.0455 μs / 12.53%**. Their wall medians increase from 9.3625 to 10.730 μs
and 9.5745 to 10.687 μs respectively. These results are retained; no causal
explanation, significance claim or release-budget waiver is inferred. The
steady 1×1 and scaled control also regress slightly. This is not a claim that
every workload or first-use latency improves.

The smaller pilot was noisier: its steady single-pixel CPU median rose 27.50%,
the scaled control rose 8.16%, and first 1080p rose 11.98%. Its original receipts
remain beside the larger final campaign. The candidate and workload were not
tuned to remove those observations. The raw report retains min/max and
nearest-rank p95; with only 12 observations per group the latter is the maximum
and is not a stable tail-latency qualification.

The optimization removes per-pixel coordinate work on matching dimensions. It
does not remove the snapshot allocation or change retained memory. No allocation
reduction, RSS/private-commit improvement, whole-application speedup, Windows
performance, GPU presentation improvement or power saving is claimed. T12/#178
budget approval and actual application/ABI measurements remain open.

## Correctness and ownership evidence

The original and candidate ASan/UBSan components each pass **35,880 assertions**.
They use the same declared fixture, actual production reader types and actual
queue, acquire, release and close methods. The independent scaling oracle covers
15 dimension combinations, including single-axis, tiny, odd, equal-area but
different-shaped, scaled and empty cases. Assertions cover every pixel, metadata,
timestamp, tightly packed planes, input immutability and independently writable
acquired storage.

Lifetime checks keep an image acquired while the producer changes and the
bounded pending queue replaces older frames. They verify FIFO survival,
`maxImages`, held native allocation retirement, callback reentry, callback-side
close, expired weak owners, existing/attached JNI environments, failed attachment
and missing environments. An explicit allocation barrier closes the reader
while conversion is in progress and verifies that the closed owner receives no
published image or callback. Synthetic VM/JNI behavior is isolated to this test.

Four compiled mutations must exit with their exact intended assertion, not a
compiler failure, crash, timeout or leak diagnostic:

| Mutation | Required rejection |
| --- | --- |
| Disable the equal-size copy | Matching extent did not use one bounded bulk copy |
| Accept a mismatched input height | Full independent pixel oracle differs |
| Omit the last four bytes | Full independent pixel oracle differs |
| Ignore the closed-owner enqueue guard | Racing close published an image |

Ordinary timing builds separately pass **35,847 assertions**. Their difference
is the deliberately omitted observer/counter and allocation-hook race checks;
the timed execution checks full pixels, metadata and retirement in every sample.
An independent agent replayed the sanitizer component and all four precise
mutants. Existing source-policy and supplier qualification controls also pass;
they remain source/metadata checks and do not prove Windows runtime behavior.

The real `SurfaceParcelTest` now keeps one acquired RGBA image while the other
ART process redraws through the same Surface transport. It acquires a second
image, verifies both full images and the held timestamp, releases the second,
and checks that the first remains unchanged. The original six-frame descriptor
reuse checks remain, followed by another descriptor-retirement assertion.
The existing mandatory Windows services track requires the new success marker.
The complete app and services test Java/D8 builds pass against the real framework
classes. Actual Windows execution and its CI aggregate remain pending.

## Reproduction

From the repository root, use new empty output paths:

```sh
python3 tools/test_image_reader_rgba.py --sanitize --negative-controls
python3 tools/test_image_reader_rgba.py --sanitize \
  --source-ref 8f53b6045e26f0bb4a482c69099d084db3e94963
python3 tools/test_image_reader_rgba.py --timing --build-output /tmp/rgba-candidate
python3 tools/test_image_reader_rgba.py --timing \
  --source-ref 8f53b6045e26f0bb4a482c69099d084db3e94963 \
  --build-output /tmp/rgba-baseline
python3 tools/benchmark_image_reader_rgba.py --blocks 6 \
  --baseline-build /tmp/rgba-baseline --candidate-build /tmp/rgba-candidate \
  --output /tmp/rgba-complete-abba.json
```

The driver rejects nonordinary builds, regenerated-source/hash mismatches,
changed executables, nonzero exits, unexpected output, inconsistent checksums,
different assertion counts and failed contracts. It writes every process receipt
as it runs, including a failed invocation before raising an error. The local
container's ptrace restrictions required `ASAN_OPTIONS=detect_leaks=0` only for
local sanitizer runs; ASan/UBSan remained active, and the CI invocation does not
disable leak detection.
