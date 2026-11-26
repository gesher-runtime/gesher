# Software AVC encoder storage reuse

## Scope and ownership

Issue #56 M04/M05/T12: reduce repeated storage allocation in the existing
software H.264 encoder. This does not add a codec, hardware capability or pixel
substitution, and does not close these broad roadmap rows.

The existing wrapper allocated a packed I420 input and a frame-sized bitstream
scratch vector for every encode call. The input copy is necessary: libavc may
retain it after the caller reuses its `VideoFrame`. The packet copy is also
necessary: an `EncodedPacket` owns its bytes independently of later encodes.
Both copies remain.

The change retains one returned input buffer and the current output scratch.
Inputs are eligible only when libavc returns their exact pointer; all other
inputs remain owned by `inputs_`. A second returned input replaces, rather than
adds to, the spare. Output scratch retains its previous successful overflow
growth, with the existing 64 MiB retry ceiling unchanged. `Drain` discards both
spare allocations; provider-owned inputs remain alive until returned or codec
destruction. Destruction retrieves and frees the provider before input storage
is destroyed.

While an active stream pauses between frames, this retains one input plus one
output scratch buffer that the old wrapper freed after every call. At ordinary
1080p sizes that is 6,220,800 bytes. The encoder's actual reference pictures,
caller frames and independently retained encoded packets are unchanged.

Contract sources are the pinned Android 16 r4
[encoder API structures](https://android.googlesource.com/platform/external/libavc/+/android-16.0.0_r4/encoder/ive2.h)
and [encoder implementation](https://android.googlesource.com/platform/external/libavc/+/android-16.0.0_r4/encoder/ih264e_encode.c),
especially its input-buffer return and synchronous output publication. The
existing configuration uses zero B frames; delayed receipt controls below do
not advertise B-frame support.

## Measured production work

The before source is `6afcb6e6`'s unchanged `soft_avc_encoder.cc`, SHA-256
`f8ec608062b562e9896554a97f31ddfc875eba093f6212211e0477aad80ebf30`.
After source SHA-256 is
`380a937aa1f0177ede8996a3fd940c2b92d21f31c1c9cdd38ee3ddebd7ad8116`.
Both execute the same actual AOSP provider:
`android-16.0.0_r4`, commit `d380d04a78c6c08702e36aca86d0fd4baee390f9`.

Four warm-up frames are excluded identically. Counters cover C++ allocations
inside the complete production `Encode` call, including its maps and packet
storage. They do not measure provider C allocations, RSS, hardware power or
end-to-end frame latency.

| Workload | Measured frames | C++ allocations before → after | Requested bytes before → after | Full-frame allocations before → after |
| --- | ---: | ---: | ---: | ---: |
| 320×240 packed | 20 | 100 → 60 | 4,709,444 → 101,444 | 40 → 0 |
| 320×240 padded/cropped | 20 | 100 → 60 | 4,710,618 → 102,618 | 40 → 0 |
| 320×240 delayed input return | 20 | 100 → 60 | 4,710,618 → 102,618 | 40 → 0 |
| 1280×720 padded/cropped | 20 | 100 → 60 | 55,537,586 → 241,586 | 40 → 0 |
| 1920×1080 padded/cropped | 8 | 40 → 24 | 49,983,335 → 216,935 | 16 → 0 |

The measured 720p/1080p allocation volume falls about 99.56%; the required input
and packet copies remain. This is an allocation result, not a claim of the same
percentage improvement in encoding speed.

## Validation and limits

- The complete production wrapper and real libavc pass 5,331 checks with the
  wrapper/observer under ASan and UBSan and the provider under ASan.
- Seven scenarios produce byte-identical before/after H.264 streams and packet
  manifests (timestamps, flags, lengths and hashes). Independent FFmpeg decode
  verifies all 125 output frames, including identical decoded-frame hashes.
- Coverage includes padded crop/stride, caller frame reuse immediately after
  submission, independently retained packets, requested IDR frames, live
  bitrate changes, EOS, repeated encoder recreation and destruction.
- Explicit test controls withhold returned-input receipts for two subsequent
  calls, inject one pre-consumption overflow, and inject one fatal failure.
  Ordinary frames still pass through the real codec. These controls validate
  wrapper ownership and error paths; they are not additional provider features.
- The unchanged baseline passes the compatibility corpus in measurement mode
  and fails the new large-allocation gate as expected.
- Production source compiles for Windows x64 and ARM64. The actual-provider
  probe is wired into the Windows media track and the Linux full CI build.
  Windows execution and ART/MediaCodec acceptance remain CI work, not local
  execution claims.
- This local container runs under tracing, so LeakSanitizer refuses to execute.
  Local commands use `ASAN_OPTIONS=detect_leaks=0`; the committed CI command
  keeps the default leak checker. Explicit ownership/teardown checks still run.

Reproduction (pass an actual pinned provider checkout):

```sh
python3 tools/test_soft_avc_reuse.py --libavc-root third_party/aosp/external/libavc --sanitize --output-dir /tmp/avc-after
python3 tools/test_soft_avc_reuse.py --libavc-root third_party/aosp/external/libavc --libavc-library /tmp/avc-after/libavc-test.a --sanitize --source-ref 6afcb6e6 --measure-only --output-dir /tmp/avc-before
cmp /tmp/avc-before/decoded.json /tmp/avc-after/decoded.json
```

Each invocation prints source identity, per-scenario counters and the contract
count. Output directories retain the native probe, real H.264 files, packet
manifests and independent decoder receipts.
