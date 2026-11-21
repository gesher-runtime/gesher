# Direct Media Foundation encoder input — 2026-11-18

## Problem and scope

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

`MfH264Encoder::Encode` allocated a zero-initialized packed I420 vector for every
video input, copied the visible source rows into it, and passed it to `NewSample`.
`NewSample` then allocated the independently owned Media Foundation buffer and
copied that complete temporary image again. Both MediaCodec encoding and the
MediaRecorder video worker reach this path.

The encoder now fills the locked MF buffer directly with the same `CopyI420`
routine. Each input still receives its own buffer and sample. No sample, source
frame, or MFT-owned storage is reused while the transform may retain it. The
`MF_E_NOTACCEPTING` path drains output and retries the same completed sample once,
as before. The encoder's output parser, actual codec, pixel selection, chroma
rounding, key-frame request, timestamp units and output ordering are preserved.

The shared sample constructor checks creation, lock, length, unlock, sample
creation, buffer attachment and timestamp/duration results. Failed construction
releases its owners and never reaches `ProcessInput`. The AAC caller retains its
ordinary byte-copy operation through the same constructor. Positive, even I420
dimensions and the MF DWORD input/output size bounds are checked before encoder
activation. Source strides, crop origins and complete plane storage are checked
before copying, and unrepresentable 100-nanosecond timestamps fail before
allocation instead of signed multiplication overflow.

These are internal storage and error-handling changes. Applications still encode
their actual supplied media. The change does not substitute content, bypass
permissions or advertise new codec capabilities. A failed video encode follows
the existing MediaCodec/MediaRecorder failure path.

## Measured input storage and copy work

The fixture includes the complete, unchanged production `mf_encoder.cc` and the
production codec-config implementation. Win32 DLL activation and the MF COM
objects are declared host adapters. Each adapter buffer owns exactly the
requested payload bytes; the sample and transform honor reference ownership.
The observer can retain inputs beyond `Encode`, refuse input, emit output or fail
an operation. It is not a real encoder and does not measure its compression work.

For **120 cropped 1920 × 1080 I420 frames**, the same fixture reports:

| Input preparation metric | Exact baseline | Direct input |
| --- | ---: | ---: |
| Payload allocations | 240 | 120 |
| Cumulative allocated payload bytes | 746,496,000 | 373,248,000 |
| Peak live packed input + temporary payload bytes | 6,220,800 | 3,110,400 |
| Explicit production `memcpy` calls | 259,320 | 259,200 |
| Explicit production `memcpy` bytes | 746,496,000 | 373,248,000 |

One complete 3,110,400-byte temporary and its copy are removed from every tested
frame. The original source frame, COM object bookkeeping, allocator overhead,
encoded output, transform-internal allocations and any driver storage are outside
these input-payload measurements. The original source and the byte oracle are
created before measurement. The sample's payload allocation is counted at the
declared MF boundary. The temporary vector's initial zero-fill also disappears,
but these counters do not separately measure zero-fill traffic.

The measured 50% reduction applies to this input-preparation allocation/copy
work, not total application RAM, total CPU time, frame rate or recording latency.
There is no additional retained cache or buffer pool.

## Verification

- **8,817 checks pass with ASan/UBSan.** This includes 256 deterministic padded
  source layouts, even output dimensions, odd/even crop origins, exact planar
  bytes, unchanged input/padding, signed representable timestamps, multiple frame
  rates, unchanged key requests and preservation of previously emitted packets.
- The actual production retry path is exercised for one and two
  `MF_E_NOTACCEPTING` results and a failed drain. The same sample is retained across
  retry. Independently retained inputs survive source mutation, later inputs and
  drain, then retire with the transform.
- Both MFT-provided and caller-provided output modes execute the production
  output parser. The fixture checks SPS/PPS publication, AUD removal, Annex B
  bytes, PTS, IDR/clean-point flags and output-event release. These observations
  use explicit synthetic output at the codec boundary, not real H.264 encoding.
- Eleven construction/metadata failures and thirteen invalid-frame/time cases
  verify rejection, lock balance, owner release and no incomplete submission.
  The shared AAC sample path, normal PCM bytes and a zero-byte sample are covered.
- The exact baseline passes **8,349 valid-input compatibility checks** in
  measurement mode. With the same valid-input suite's allocation/copy budgets
  enabled, it fails **exactly four** checks: allocation count, allocated payload
  bytes, peak payload bytes and copy bytes. Baseline-unsafe invalid-pointer and
  failed-creation paths are explicitly excluded from that negative control.
- The complete changed production translation unit compiles for Windows x64 and
  ARM64 with the CI-pinned LLVM-MinGW `20261006`, C++20, `-Wall -Wextra -Werror`
  and `-fno-exceptions`.
- The new host gate runs unconditionally in the existing media-source CI
  preflight. `git diff --check` passes.

The host adapter also preserves Windows' 32-bit `unsigned long` formatting for
HRESULT diagnostics; its Linux host otherwise uses a 64-bit `unsigned long`.
Only that ABI formatting boundary and Win32/MF are adapted; the production
translation unit is not rewritten by the test driver.

Local runs set `ASAN_OPTIONS=detect_leaks=0` because this environment cannot run
LeakSanitizer process inspection under ptrace. ASan/UBSan remain fatal; the CI
command keeps its normal sanitizer defaults. Reference/object counts additionally
check every adapted COM owner, but do not establish real Windows allocator
behavior.

### Reproduce

```sh
python3 tools/test_mf_encoder_input.py --sanitize
python3 tools/test_mf_encoder_input.py --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2 --measure-only --sanitize
# Expected nonzero exit: four allocation/copy budget failures.
python3 tools/test_mf_encoder_input.py --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2 --valid-only --sanitize
```

Complete production compile, for each of `x86_64` and `aarch64`:

```sh
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" -std=c++20 \
  -DGESHER_MEDIA_STANDALONE -DNOMINMAX -Wall -Wextra -Werror -fno-exceptions \
  -c compat/media/encode/mf_encoder.cc -o mf-encoder-x64.o
"$LLVM_MINGW/bin/aarch64-w64-mingw32-clang++" -std=c++20 \
  -DGESHER_MEDIA_STANDALONE -DNOMINMAX -Wall -Wextra -Werror -fno-exceptions \
  -c compat/media/encode/mf_encoder.cc -o mf-encoder-arm64.o
```

Fresh Windows/ART recording, a real Media Foundation H.264 encoder, physical
camera/GPU behavior and application-wide timing/RSS measurements remain platform
validation gates. Cross-compilation and adapted host execution do not prove them.

## Pending work and contract references

Pending [#104](https://github.com/gesher-runtime/gesher/pull/104), inspected at
`0db39cdc532f4d93c77f57a0a784928f96be7bbd`, changes bitrate policy adjacent to this
encoder method. It leaves the packed input/sample-construction path unchanged.
A three-way file merge of that head's `mf_encoder.cc`, the stated main baseline
and this change is clean. This is not combined-branch runtime validation; that
PR's API additions still require its corresponding headers and tests. Pending
[#168](https://github.com/gesher-runtime/gesher/pull/168) changes video decoding,
which is a separate path from the encoding change here.

Primary contracts:

- [MF buffer/sample ownership](https://learn.microsoft.com/en-us/windows/win32/medfound/workingwithmftmediabuffersandsamples).
- [Lock pointer lifetime and matching Unlock](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfmediabuffer-lock).
- [Valid buffer length](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfmediabuffer-setcurrentlength).
- [ProcessInput sample retention and NOTACCEPTING](https://learn.microsoft.com/en-us/windows/win32/api/mftransform/nf-mftransform-imftransform-processinput).
- [Encoder processing and output allocation](https://learn.microsoft.com/en-us/windows/win32/medfound/processing-data-in-the-encoder).
- [Android MediaCodec input/output contract](https://developer.android.com/reference/android/media/MediaCodec).
