# Target-sized MediaMetadataRetriever raster conversion (MEDIA-07, first round)

Refs #178. Baseline: `8b316aebf09ab46d5407e955bf86ed748a1e739f`.

## Observable contract and scope

`getFrameAtTime`, `getScaledFrameAtTime`, and indexed frame lists retain the existing selected frame, decoder crop, clockwise presentation rotation, sample/display aspect fit, ARGB8888/RGB565 preference and fallback, and `BitmapParams.outActualConfig` behavior. Scaling remains a linear RGB filter after the original BT.601 limited-range conversion and clipping. RGB565 conversion happens **before** filtering, preserving the old operation order. The caller's data source and decoded picture remain the inputs; no fixture or replacement image enters ordinary application behavior.

The source-size initial Bitmap and three full-size conversion arrays are removed. The hwui bridge allocates only the final Android Bitmap. The production renderer holds one ARGB source row and up to two source rows in the selected Bitmap format, converting only rows needed by target samples. Adjacent windows retain the shared row. A borrowed Skia image is destroyed before those pixels change and before its storage is freed. When either source dimension reaches Skia's signed-16-bit sampler range, a single float output row keeps the corresponding high-precision sampling path.

Contiguous source rows retain the existing `RowToRgba` vectorized conversion, including its Clang x86-64 AVX2 dispatch. Odd crops convert their first pixel separately before the aligned chroma pairs. Channel reordering and 180-degree reversal happen in the same row buffer; quarter-turn rotations retain the coordinate-based path. No extra pixel buffer is introduced for the fast path.

The final Bitmap keeps sRGB, default density, requested alpha/premultiplication semantics, and the previous mutability distinction: an unscaled source Bitmap is immutable; a scaled result is mutable. Invalid final dimensions still reach `IllegalArgumentException`; invalid metadata is rejected before division or out-of-range conversion. Initial Bitmap allocation failure returns null, while failure of a scaled result allocation reports `OutOfMemoryError`, matching the corresponding former allocation stages. Invalid/truncated I420 planes are rejected before pixel reads. This round does not add general allocation-fault injection or claim every internal Skia OOM path was exercised.

Decoder/extractor reuse, source-generation caching, complete seek/flush/release cache policy, machine-profile latency/working-set measurements and the full MEDIA-07/#178 acceptance remain open. Source lifetime, frame PTS/index/EOS selection and extractor behavior are unchanged.

## Primary contract references

- [MediaMetadataRetriever API](https://developer.android.com/reference/android/media/MediaMetadataRetriever): scaled output fits the supplied bounds while retaining aspect ratio; BitmapParams selects supported output configurations.
- [Pinned Bitmap.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/graphics/java/android/graphics/Bitmap.java): `createScaledBitmap` uses the filtered Bitmap drawing path and carries Bitmap properties.
- [Pinned native Bitmap creation](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/jni/Bitmap.cpp) and [GraphicsJNI conversion](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/jni/Graphics.cpp): ARGB integer input is an sRGB BGRA/unpremultiplied pixmap, converted into the selected native format.
- [Pinned SkiaCanvas](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/SkiaCanvas.cpp): Bitmap source/destination rectangles use `drawImageRect` with the fast source-rectangle constraint.
- [Pinned Skia sampler](https://android.googlesource.com/platform/external/skia/+/android-16.0.0_r4/src/shaders/SkImageShader.cpp): image bounds influence fixed/floating-point sampler selection.

## Pixel oracle and precision

`tests/media/retriever_pixels_test.cc` compiles the **complete production renderer**. Its independent reference materializes the entire rotated ARGB image, converts it into a full selected-format Bitmap, and draws that full image with the original float scale and actual pinned Skia `drawImageRect` path. The reference uses forward rotation; production samples inverse rotation. It checks every output pixel, not only center colors.

Local x86-64 CPU execution used Android tag `android-16.0.0_r4`, Skia commit `22f5edb6c1bf350d1d9d67bb429db1a5539f3c05`, an RGBA N32 configuration and a locally built raster-only library. The host config excludes Android logging, graphics/window integration and GPU services; the Windows CMake target links the actual product Skia instead. The library and source hashes and GN configuration are in the adjacent JSON evidence.

| Format | Cases | Pixels compared | Different pixels | Largest packed-channel difference |
| --- | ---: | ---: | ---: | ---: |
| ARGB8888 | 386 | 4,642,840 | 0 | 0 |
| RGB565 | 386 | 4,642,840 | 85 | 1 |

The RGB565 differences are approximately 0.00183% of compared pixels, each one 5-bit red/blue or 6-bit green level at a floating-point quantization boundary. The comparison permits at most one output-format channel step for scaled output; unscaled conversion must be exact. A 5-bit/6-bit step is not a one-unit change in the expanded 8-bit channel. The public Android API does not promise a bit-identical internal sampler across implementations, but the independent reviewer must still assess these measured differences against the issue's pixel-preservation requirement. No broader tolerance, crop displacement, missing edge, rotation change or filter-kernel change is accepted.

Earlier prototype variants failed this oracle: a newer-Skia-inspired 16.16 pre-rounding produced an 11-unit ARGB difference on an adversarial mixed-scale case, and translating clipped windows in full destination coordinates produced a 5-unit difference in a 4K case. Neither variant is in this change. The final path retains the original float inverse coordinate, keeps each destination row at its local origin, and uses the high-precision row only where the full source would require it. A Python Skia prototype was exploratory and is not acceptance evidence.

Cases cover all four rotations plus unchanged unknown-rotation behavior, odd and padded I420 crops, nonuniform pixels, single-row/column images, up/down scaling, 4K thumbnails, very wide/tall images above 32767 pixels, 256 seeded varied dimensions per format, malformed buffers/strides/crops, dimension overflow, and 100 repeated 4K thumbnails. Final sanitized run: **4,648,625 checks passed**.

## Resource result and limits

For a 3840x2160 decoded frame and 160x90 ARGB result, the actual renderer reports 46,080 bytes of row pixel scratch and the final Bitmap holds 57,600 bytes. It converts at most 691,200 source pixels (180 rows), versus 8,294,400 for full-frame conversion. RGB565 row storage uses fewer bytes. The counters reflect the owned pixel buffers, not all small Skia objects, allocator bookkeeping, the decoder's I420 storage, or process working set.

From the baseline source, the removed Java int array plus native RGBA/ARGB arrays account for 99,532,800 bytes at 4K, and the removed ARGB source Bitmap another 33,177,600 bytes. Those are allocation-size arithmetic, **not** an observed process peak. The final required output Bitmap remains. There is no persistent conversion cache; all scratch and borrowed images retire on return. Repetition tests establish bounded per-call pixel storage, not a measured Windows heap plateau or decoder-cache benefit.

## Independent unscaled-path regression and correction

Independent QA compared the actual old `I420ToRgba` conversion, full native RGBA/ARGB arrays, a simulated Java int-array copy and pinned-Skia Bitmap write with the complete new renderer. Its initial 720p even-crop result was 6.565 ms versus 8.474 ms, a 29% regression. The author reproduced the same unchanged fixture at 6.690 ms versus 8.509 ms before correction. The cause was the initial renderer's per-pixel scalar conversion and coordinate division in a path where the old converter used vectorized contiguous rows.

After restoring that existing row helper within the bounded buffer, the unchanged fixture produced these paired upper medians from ten alternating samples per path (GCC 14.2, `-O2`, same Linux host and pinned CPU Skia library):

| Unscaled input | Crop offset | Old full pipeline (ms) | Revised row pipeline (ms) | Revised / old |
| --- | ---: | ---: | ---: | ---: |
| 1280x720 | 0 | 6.508 | 6.318 | 0.97 |
| 1280x720 | 1 | 9.792 | 6.163 | 0.63 |
| 3840x2160 | 0 | 89.479 | 59.750 | 0.67 |
| 3840x2160 | 1 | 88.465 | 56.170 | 0.63 |

Each pair checks exact output pixels before timing. The narrow probe resolves the observed scalar-conversion regression; the common 720p case is approximately equal in this host sample. These timings do not measure decoder or JNI execution, Windows performance, machine-profile distributions or whole-process memory, and they do not substitute for the issue's performance acceptance. The adjacent JSON records the independent fixture hash and both author reproduction runs. The full sanitized pixel/allocation oracle still reports exactly the same format results after this correction.

## Validation and reproduction

```sh
python3 tools/test_retriever_pixels.py --skia-root /path/to/pinned/skia \
  --skia-library /path/to/pinned/skia/out/qa/libskia.a
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_retriever_pixels.py \
  --skia-root /path/to/pinned/skia --skia-library /path/to/pinned/skia/out/qa/libskia.a --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_retriever_frame_contracts.py --sanitize
```

The host pixel suite passed with address/undefined sanitizers around the complete production renderer and fixture. The linked Skia library is not sanitizer-instrumented. Its default no-RTTI ABI means vptr sanitizer coverage is unavailable. This executor cannot run LeakSanitizer under ptrace, so only leak detection was disabled for local runs. Existing reader/selection/display regressions passed **164/164**. The final renderer compiled with the cached llvm-mingw x86-64 Windows compiler and warnings as errors, including the same Skia unknown-pragma suppression used by the product. This proves compilation of that translation unit, not linkage/execution of the whole Windows runtime.

The new `VideoActivity` gate compiled against cached framework classes and dexed with D8. Its five generated MP4 fixtures share identical decoded I420 SHA256 `e46ccfb8c7f7659d44658f24a464ba0ca53803c78b256cb8f6a565020db31d6f`; remuxing adds 90/180/270 rotation or 3:2 sample aspect. It checks every actual native retriever pixel against public Bitmap rotation/scaling, all three preference cases including fallback, and Java Bitmap density/config/alpha/premultiplication/mutability. It avoids double-filtering the SAR oracle. Generation uses FFmpeg's explicit display-rotation input option, with its sign converted to Android's clockwise convention.

`38-media.ps1` now requires both the native pinned-Skia executable and the 15 public-ART cases. The executable is a default CMake target collected with other native CI fixtures. These Windows/ART gates are wired and compile-checked locally but were **not executed locally**. Actual execution, whole-runtime native Bitmap bridge linkage, platform allocator failure paths, real codec/device profiles and end-to-end performance measurements await CI or a suitable runtime. The fixtures are isolated test inputs and are not packaged into ordinary application behavior.
