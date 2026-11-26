# Transfer exact packed HEVC output without a second frame copy

## Scope and result

This is a software HEVC decoder leaf supporting [#56 M08/T12](https://github.com/gesher-runtime/gesher/issues/56). After actual libhevc decoding finishes, Gesher previously copied each caller-owned I420 plane into a second `VideoFrame` allocation. The new path transfers the existing allocation only when all three returned pointers, strides and the complete allocation size describe exactly that packed frame. Padded, cropped, oversized and foreign-plane layouts retain the existing `CopyPicture` path.

Across the five complete real-stream workloads below, wrapper frame copies fall from **173,166,984 to 38,687,112 bytes**: **134,479,872 bytes, or 77.66%, removed**. The eligible 4K clip alone eliminates 99,532,800 copied bytes over eight displayed frames. Every displayed frame remains byte-identical to independent FFmpeg decoding.

This is a reduction in the wrapper's actual copied-byte count. Provider decode/conversion, compressed-input work, downstream rendering and output consumer ownership remain. There is no whole-pipeline zero-copy, CPU, FPS, latency or RSS claim. Allocation counts do not decrease. Steady eligible decode calls hold one frame-sized allocation instead of two, while the full-workload observed peak is unchanged because drain retains a returned frame while making its final empty output call. Both measurements are reported below.

## Actual provider ownership

The pinned provider is AOSP `external/libhevc` at `c83a76b084498d55f252f48b2e3786804cdf24b7` (`android-16.0.0_r4`). Gesher already creates it with `u4_share_disp_buf = 0`, `IV_YUV_420P`, and `u4_keep_threads_active = 0`.

The provider's [complete decode implementation](https://android.googlesource.com/platform/external/libhevc/+/c83a76b084498d55f252f48b2e3786804cdf24b7/decoder/ihevcd_decode.c) establishes the boundary:

- In non-shared mode, `ihevcd_fill_outargs` returns the three pointers from this call's `s_out_buffer`, rather than its internal prediction buffers.
- Normal decode writes those output planes through `ihevcd_fmt_conv`, releases its internal display reference, and joins the processing workers before returning. Reference pictures used for future prediction remain separate.
- Flush mode also converts into the caller's planes and releases the display reference synchronously before filling the output arguments.

The test links that real provider and observes the input/output pointer identities on every returned frame. It also immediately modifies every caller-visible returned frame in a dedicated stream, then checks that subsequent real predicted frames still match FFmpeg. Earlier modified outputs stay intact after later decode, drain and destruction. This establishes the consumer's independent ownership in addition to retained-frame read stability.

The production change checks Y, U and V strides, `buffer_.size()`, and all three exact pointer offsets before `swap`. The next `Call` allocates its own output buffer. Any failed condition uses the unchanged row-copy conversion. There are no new views, reference-counted aliases, caps, frame drops or success paths.

## Separate timestamp prerequisite

Main `d38b5549dc3963f8d491e9205f9b2743bb6d3745` fails the strict real-provider resolution-change test: the first 720p frame after the smaller stream inherits timestamp `366670` instead of `10000007`. `Reset` clears the map while the triggering access unit is about to be retried.

Separate commit `2c050a982639f029a3bfb9cafd1d28eec2c1f7ad` applies the same scoped marker-restoration approach as [AVC #342](https://github.com/gesher-runtime/gesher/pull/342), to HEVC's distinct decode function. After old output is drained and reset completes, it restores **only the still-unconsumed access unit's marker**. It does not revive old mappings, change display order, impose monotonic input timestamps or alter the oracle. Real small-to-large and large-to-small streams then pass every pixel and independent timestamp check.

The performance comparison uses this corrected timestamp-only commit as its baseline, so the ownership change is measured separately. The original failed run remains failed.

| `compat/media/video/soft_video.cc` source | SHA-256 |
| --- | --- |
| Original main | `4f01ad58f426d114f099d567b156f772ac2e7b40ef039d6841096c4444176ef9` |
| Timestamp-only baseline | `5ac9728cae83b041dc88284fce46bccc35acd197ab170dc2a9682dd59b67a9be` |
| Ownership-transfer candidate | `fa5a94a85f741baa7e88765ad138ac55f5c4c34047435a91d9085104ba35ead7` |

## Complete-stream resource results

FFmpeg 7.1.5 encodes real Main-profile I420 access units with fixed two-B-frame GOPs and independently decodes the streams. The fixture uses `ffprobe`'s display order and packet positions to assign nonmonotonic input presentation timestamps, then requires exact display timestamps and full-frame bytes. This provider does not assign `e_dec_pic_type`; the fixture verifies actual delayed marker identities and drained output instead of interpreting that unused field.

The counter covers complete product `Decode` and `Drain` calls. Large C++ allocations at least as large as the clip's visible frame are tracked across allocation and later deletion. Provider C allocations, codec initialization state, small metadata, test inputs and oracle storage are outside this counter. The copied-byte hook is in the complete product translation unit; provider-internal conversions are not included. The test clears consumed output between ordinary calls; the separate retention corpus intentionally holds all outputs.

| Visible dimensions / frames | Actual wrapper copied bytes, before → after | Full-workload peak observed large bytes, before → after | Large allocations, before → after |
| --- | ---: | ---: | ---: |
| 384×256 / 12 | 1,769,472 → 0 | 294,912 → 294,912 | 13 → 13 |
| 318×238 cropped / 12 | 1,362,312 → 1,362,312 | 251,766 → 251,766 | 13 → 13 |
| 1280×720 / 24 | 33,177,600 → 0 | 2,764,800 → 2,764,800 | 25 → 25 |
| 1920×1080 padded / 12 | 37,324,800 → 37,324,800 | 6,243,840 → 6,243,840 | 13 → 13 |
| 3840×2160 / 8 | 99,532,800 → 0 | 24,883,200 → 24,883,200 | 9 → 9 |

The cropped width has padded provider rows, and 1080p's scratch height is rounded to 1088. Neither is an exact packed allocation, so both retain their previous copy cost. Resolution decreases can also leave an oversized buffer, which uses the same conservative fallback. No benefit is claimed for those layouts.

For output-producing calls after the first six access units, the eligible steady peaks are 294,912 → 147,456 bytes at 384×256, 2,764,800 → 1,382,400 at 720p, and 24,883,200 → 12,441,600 at 4K. These are **50% reductions for those calls**, not lower overall high-water marks. The frame allocation has moved from `CopyPicture` to the next provider call, so the per-output allocation count and required vector initialization work are not claimed as savings.

[Raw source-tagged measurements, fixture hashes and commands](software-hevc-output-2026-11-23.json) retain both versions and the original timestamp failure. Deterministic byte/storage counts need no timing confidence interval; unmeasured CPU, latency, RSS and Windows runtime results are explicit.

## Validation boundaries and required CI

The candidate passes **12,098 checks**, including all **68 full frames byte-identical to FFmpeg**. The timestamp-only baseline passes 12,092; the six additional candidate checks are the two resource assertions for each of the three eligible layouts, not additional feature claims.

The complete current production `soft_video.cc` and `video_decoder.cc` are compiled into the fixture. Selected libhevc code comes from the exact CMake source list and production x86 feature configuration. Provider C uses ASan; the product wrapper and fixture also use UBSan. Null `Format` metadata is an explicit boundary: actual headers are learned from real access units. Real VPx/dav1d headers are present, but unselected decoder entry points abort if invoked; none simulates successful decode.

Retained output is checked after every later access unit, drain, flush, a second decode after drain, decoder destruction, injected failure before consuming input, and real resolution changes in both directions. A dedicated consumer-write stream checks prediction-buffer independence. Foreign storage substitutes Y, U, V and all three planes in turn while preserving the real decoded pixels; original scratch and the foreign buffer are poisoned at their lifetime boundaries, so bypassing a pointer check cannot silently pass the pixel oracle. Natural cropped and padded streams exercise the existing fallback.

Four separately compiled production mutants fail their specific assertions: always copying fails the copied-byte budget; dropping only Y, only U or only V pointer identity fails the full-pixel hash when that particular plane is foreign. The helper requires the named assertion and expected assertion-abort exit; a compiler error, unrelated crash or timeout does not count. All four controls pass and run in the required Linux CI step.

Both x64 and ARM64 Windows production translation units compile with pinned `llvm-mingw-20261006`. All real x64 libhevc objects and the complete Windows fixture link. This is compile/link evidence, not Windows execution. The required Windows media track invokes the executable and the generated compressed fixtures unconditionally; it does not skip this probe when an optional app artifact is absent.

The 21 release-workflow, 6 cache-policy and 3 workflow-metadata checks pass. The three dependency working trees are clean at their recorded pinned revisions.

`ci.yml` runs the real provider/oracle helper after installing FFmpeg and fetching pinned AOSP sources. It carries only compressed fixtures, dimensions/timestamps/pixel hashes and their manifest through `framework-assets`; large raw oracle frames remain local to the host test. The separate existing immutable framework startup repair from [#346](https://github.com/gesher-runtime/gesher/pull/346) replaces inaccessible `06d175a8` with published `6afcb6e6`; both complete workflow blobs are identically `834692396e307a6cd9251c527b7a91ab402d13ac`. No runner, permission, supplier qualification or existing gate changes.

**Actual Windows/ART execution and full fork CI remain pending at submission.** This leaf does not complete M08's hardware, asynchronous MFT, HDR or broader media-acceleration work, or T12's application CPU/RAM/latency measurements. It does not implement M04's persistent input/LinearBlock or M05's remaining runtime controls. Existing media capabilities, formats, failure reporting and queue ownership remain required by the complete workflow.

## Reproduction

```sh
python3 tools/test_soft_hevc_output.py \
  --libhevc-root third_party/aosp/external/libhevc \
  --vpx-root third_party/aosp/external/libvpx \
  --dav1d-root third_party/aosp/external/libdav1d \
  --sanitize --negative-controls --output-dir out/media-hevc-output-fixtures

python3 tools/test_soft_hevc_output.py \
  --libhevc-root third_party/aosp/external/libhevc \
  --vpx-root third_party/aosp/external/libvpx \
  --dav1d-root third_party/aosp/external/libdav1d \
  --source-ref 2c050a982639f029a3bfb9cafd1d28eec2c1f7ad \
  --sanitize --measure-only --output-dir out/media-hevc-output-before
```

The constrained local tracer requires `ASAN_OPTIONS=detect_leaks=0`; committed CI retains normal LeakSanitizer defaults. To reuse a built real-provider archive, pass `--libhevc-library`; `--reuse-fixtures` verifies fixture hashes before running. The Windows media gate uses:

```powershell
./media_soft_hevc_output_test.exe gate ./media-hevc-output-fixtures
```
