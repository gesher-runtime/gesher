# D3D11 video readback without an intermediate CPU image

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2` (upstream `main`).

## Production behavior

The hardware decoder's `ExtractFrame` used to read a staging texture into a new,
tightly packed NV12 vector and then convert that vector into an independently
allocated I420 `VideoFrame`. `ExtractRgba` similarly copied the complete NV12
image before its existing color conversion. Every frame paid for an additional
allocation, zero initialization and full-image copy. The temporary was discarded
as soon as that frame completed.

Both conversions now read the mapped staging texture directly, honoring its row
pitch, and write only the final owned output. The explicit `ExtractNv12` API still
returns its own tight copy. The existing GPU-to-staging `CopyResource` and
synchronous `Map` remain. No GPU texture or mapped pointer escapes to an Android
consumer, and there is no new persistent frame cache or pixel pool.

A scoped readback object pairs each successful `Map` with exactly one `Unmap`,
including invalid mapped-layout errors. Supported input is a single-subresource,
non-multisampled NV12 texture with even, representable dimensions and a valid row
pitch. `Decode` now propagates a failed texture extraction after releasing the
sample and media buffer; it does not append an empty frame and report success.
The existing texture acquisition mechanism is unchanged.

Frame dimensions, tight I420 planes, full-frame crop, timestamps, RGBA channel
order and the existing BT.601 integer arithmetic are preserved. The application
still receives decoded bytes from its requested media. No permissions, capability
advertisement or application workflow changes are introduced.

## Measurements

The driver compiles the actual production staging allocator, conversion/extraction
methods and `Decode` against explicit device/context/texture and Media Foundation
boundaries. It also compiles the unchanged production `VideoFrame::Allocate` and
`Nv12ToI420` bodies. The adapter's texture storage and GPU copy simulation are
excluded from allocation and `memcpy` counters. These measurements describe CPU
output and temporary storage, not process RSS, GPU memory, playback FPS or real
readback latency.

For **120 frames at 1920 × 1080**, with an already allocated staging resource:

| Workload | Baseline | Updated |
| --- | ---: | ---: |
| Fresh I420 output: allocations | 240 | 120 |
| Fresh I420 output: cumulative allocated bytes | 746,496,000 | 373,248,000 |
| Fresh I420 output: peak live tracked bytes | 6,220,800 | 3,110,400 |
| Fresh I420 output: explicit CPU `memcpy` bytes | 622,080,000 | 248,832,000 |
| Fresh RGBA output: allocations | 240 | 120 |
| Fresh RGBA output: cumulative allocated bytes | 1,368,576,000 | 995,328,000 |
| Fresh RGBA output: peak live tracked bytes | 11,404,800 | 8,294,400 |
| Fresh RGBA output: explicit CPU `memcpy` bytes | 373,248,000 | 0 |
| Reused I420 destination: temporary allocations / bytes | 120 / 373,248,000 | 0 / 0 |
| Reused RGBA destination: temporary allocations / bytes | 120 / 373,248,000 | 0 / 0 |

The regular `Decode` path creates a fresh final `VideoFrame`; the fresh-output
measurements apply to that pattern. Reused-output measurements describe repeated
calls with a caller-owned destination, not a new decoder pooling policy.

The removed intermediate is 3,110,400 bytes per 1080p frame. At 60 frames/s that
would avoid 186,624,000 bytes/s of explicit CPU copy writes, plus the corresponding
reads and the vector's initialization. That is an analytical byte rate derived
from the measured operation, not a measured application throughput gain.

## Verification

The updated host suite passes **18,568 checks** with ASan and UBSan. Its coverage
includes 256 seeded even-sized layouts, repeated staging reuse, tightly packed and
padded rows, exact Y/U/V and RGBA output, signed timestamp extremes, independent
output ownership, creation/Map failures, invalid descriptors and pitches, and
recovery after errors. On `Unmap`, the adapter overwrites its mapped pixels so an
escaped alias cannot pass the output checks. `DepthPitch` deliberately does not
describe the byte count, ensuring conversion depends only on the documented row
layout. The complete `Decode` method is exercised with both MFT-provided and
caller-created output samples; readback errors retain already queued frames and
release all acquired sample, buffer, service and event objects.

The exact baseline passes **18,312 checks** in the same valid-layout compatibility
scope. New invalid-layout checks are omitted from that baseline mode because the
old code did not safely validate those layouts. Enforcing the improved resource
bounds against the baseline fails the intended 16 allocation/copy gates.
A separate negative control that restores the ignored `ExtractFrame` result in
the updated `Decode` fails eight error/publication assertions, with no invalid
memory accesses needed to demonstrate the regression.

Reproduce the portable gates:

```sh
python3 tools/test_d3d11_readback.py --sanitize
python3 tools/test_d3d11_readback.py \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2 \
  --compatibility-only --measure
```

The driver is included in the mandatory Linux preflight of `.github/workflows/ci.yml`.
`--benchmark` additionally prints informational host timings; they are never a
pass/fail criterion. `--compile-only --output FILE --cxx COMPILER` builds the same
boundary fixture without claiming to execute it on another platform.

The complete changed `d3d11_decoder.cc` translation unit compiles for Windows x64
and ARM64 using the CI-pinned LLVM-MinGW `20261006`, patched Android 16
`libnativehelper` JNI headers, and `-Wall -Wextra -Werror -fno-exceptions`. No warning
was suppressed to obtain those results. Local sanitizer execution disables only
LeakSanitizer's process inspection because the executor is under ptrace; ASan and
UBSan remain fatal and the CI command retains its ordinary sanitizer defaults.

## Contract evidence and limits

Microsoft documents the NV12 even-dimension and plane layout rules in
[DXGI_FORMAT_NV12](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format).
The mapped layout follows
[D3D11_MAPPED_SUBRESOURCE](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_mapped_subresource):
row pitch can include padding and 2D `DepthPitch` must not be treated as an image
size. The scoped lifetime follows
[Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
and [Unmap](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-unmap).
The single-subresource admission follows the matching dimensions and resource
topology required by
[CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource).
Android's [MediaCodec output contract](https://developer.android.com/reference/android/media/MediaCodec)
still receives the same independently owned decoded image through Gesher's
existing output pipeline; the direct mapping is an internal conversion change.

This work does not implement texture-array slice selection, change the existing
`IMFGetService` acquisition route, or repair the broader decoder negotiation/drain
state machine. An unsupported array cannot be copied by this existing whole-resource
readback route and now fails explicitly. It does not establish those features as
working. Native Windows execution, physical GPU behavior, Media Foundation codec
output and Windows/ART playback remain required platform validation. Host byte
comparisons and cross-compilation do not replace those checks.
