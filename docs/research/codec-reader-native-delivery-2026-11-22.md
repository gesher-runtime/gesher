# Native MediaCodec delivery to ImageReader

## Scope and contract

Baseline: `751d82310f7c4c470c098ccebfd5d2d04699a0cd`.
Refs #56 (M05/T12; ImageReader integration) and #178 (MEDIA-04, partial).

MediaCodec already retains a decoded I420 frame while `releaseOutputBuffer`
delivers it. The old ImageReader path creates a complete NV12 vector, copies it
into a Java `byte[]`, calls back into native code, and converts the array into the
reader's final I420 storage. JNI pinning can add another copy, depending on the VM.

The new native entry writes directly into the final Image allocation. Matching
geometry uses bulk plane/row copies. Scaling keeps the previous independent
nearest-neighbour luma and chroma sampling, including odd crop origins and the
previous even visible extent. Output dimensions, timestamp (including zero and
negative explicit values), dataspace, strides and status translation are retained.
The existing reader queue, pending-image replacement and `maxImages` acquisition
checks remain authoritative. Close is checked before allocation and again by the
existing enqueue path after conversion. Event delivery remains outside locks.

The final Image storage is copied deliberately. Application-visible plane buffers
must not alias the decoder frame or later queued images. This keeps the existing
[ImageReader acquisition/release contract](https://developer.android.com/reference/android/media/ImageReader#acquireNextImage())
and avoids retaining an entire decoder allocation after its visible crop has
been delivered. No native capture transport, camera policy, hardware capability,
GPU readback, codec decoding or public API is changed. RGBA readers that expose a
native window still use that existing window path.

## Reproducible evidence

```sh
python3 tools/test_codec_reader_delivery.py --sanitize \
  --compare-ref 751d82310f7c4c470c098ccebfd5d2d04699a0cd \
  --json codec-reader-results.json
```

The fixture compiles the actual codec delivery wrapper and the native reader
lookup, converters, enqueue, acquisition, release and close methods together.
JNI object/array operations and ANativeWindow are declared host boundaries.
The Java pin adapter makes **no extra copy**, so reported savings exclude any
VM-dependent pin copy. Native allocation counts use the real C++ allocator;
Java array allocation and `SetByteArrayRegion` payload are recorded separately.

For 12 frames at each size:

| Dimensions | Native allocated bytes, before → after | Java array bytes, before → after | JNI copy bytes, before → after |
| --- | ---: | ---: | ---: |
| 1280×720 | 33,180,480 → 16,591,680 | 16,588,800 → 0 | 16,588,800 → 0 |
| 1920×1080 | 74,652,480 → 37,327,680 | 37,324,800 → 0 | 37,324,800 → 0 |
| 3840×2160 | 298,601,280 → 149,302,080 | 149,299,200 → 0 | 149,299,200 → 0 |

Every output checksum and retained image size matches. At 4K, the eliminated
native and Java intermediates account for **24,883,200 allocated bytes per
frame**; the final independent 12,441,600-byte Image remains. These are bounded
path-level work/allocation measurements, not a Windows latency, resident-memory,
whole-application speed or FPS claim. Source hashes and raw counts are in
[`codec-reader-native-delivery-2026-11-22.json`](codec-reader-native-delivery-2026-11-22.json).

## Validation and limits

- **200,977 candidate checks**, including 192 YUV/PRIVATE/RGBA pixel cases with
  nonuniform planes, crop/padding, odd dimensions, scaling in both directions and
  one-pixel targets. Baseline output also passes the pixel/lifetime oracle.
- Real queue/acquisition methods cover held writable image isolation, pending
  replacement, acquisition exhaustion and release, reentrant close, close racing
  an unlocked conversion, callback exceptions, malformed source storage and 200
  close/reopen cycles. Native held-storage checks do not claim that Java Image
  remains usable after `ImageReader.close()`.
- The old source compiles and fails six new allocation/copy assertions, proving
  the gate distinguishes the old implementation.
- Existing Surface/codec ownership tests: **25 + 194 checks**. Existing codec
  output-storage fixture: **999 checks**. Camera conversion fixture:
  **2,322,078 checks**. All passed with ASan/UBSan.
- This container runs under ptrace, where LeakSanitizer cannot inspect threads;
  local sanitized runs set `ASAN_OPTIONS=detect_leaks=0`. ASan and UBSan remain
  enabled. The added Linux CI command uses normal sanitizer defaults.
- The updated recordapp compiles to Java classes and DEX against actual Android
  16 framework/core build inputs. Its existing Windows/ART decoder→ImageReader
  fixture now holds the first image through later decoded frames and requires
  unchanged plane bytes. Existing frame, color and timestamp gates remain.
- Full Windows product compilation, Windows/ART execution, physical video
  devices and complete #178 workload/soak acceptance must be established by CI
  and target-hardware qualification; they were not executed locally.
