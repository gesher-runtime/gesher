# Incremental HttpEngine Brotli: implementation and traffic evidence

Date: 2026-11-23. Related work: #56 N48/T12 and #178.

## Result and scope

`HttpEngine.Builder.setEnableBrotli(true)` previously returned the builder without
changing the engine. It now enables the pinned AOSP RFC 7932 decoder for unary
requests, engine-owned URL connections and HTTP/2 bidirectional streams. The
default remains disabled, as documented by the Android API. The decoder does
real incremental native work; no body, codec, transport or success result is
substituted.

The public-API workload delivered the same 13,166,720 decoded bytes in ten real
HTTP responses. The unchanged base engine transferred 417,870 response-body
bytes using gzip; the candidate transferred 191,340 using Brotli: **54.21% less
response-body traffic than the already compressed baseline**. Every response is
length-checked and SHA-256-checked after decoding. This is a deterministic,
compressible synthetic JSON corpus, not a claim about every application or
internet response. The server chooses its representation from the actual
`Accept-Encoding` request header; it does not switch implementations based on
which revision is under test.

| Measurement | Unchanged base | Candidate |
| --- | ---: | ---: |
| Requests | 10 | 10 |
| Decoded bytes | 13,166,720 | 13,166,720 |
| Encoded response-body bytes | 417,870 | 191,340 |
| Brotli responses negotiated | 0 | 10 |
| Bytes for one response | 41,787 gzip | 19,134 Brotli |

The gain depends on a server offering Brotli and on its compression settings and
content. Compression is performed before the measured transfer: no server CPU,
client latency, energy or universal speedup is claimed. Gzip uses the JDK's
default `GZIPOutputStream`; Brotli fixtures use the pinned encoder at quality 5,
window 22. The fixture generator verifies the checked-in byte streams exactly.

This completes a bounded Brotli implementation leaf. N48 remains open for its
other requirements, including QUIC/H3, DNS/migration, certificate pins and full
metrics/tagging qualification. T12 real-application/ABI/hardware budgets remain
open. No device or Windows runtime pass is inferred from Linux host tests.

## Ownership, streaming and protocol behavior

The production build links only Brotli `common` and `dec`. The encoder is fetched
only to independently reproduce test vectors. JNI follows the existing built-in
library registrar and the actual nativehelper SysV entry adapters. The upstream
MIT license is fetched and included in the networking notice inventory.

Each active decoder owns a 32 KiB Java input chunk and two 32 KiB native region
buffers, plus the provider's bounded RFC sliding window and bookkeeping. The
standard window may approach 16 MiB for a stream that requests it; this is an
explicit memory tradeoff, not a claim of constant tiny memory. Nonstandard
large-window decoding, custom dictionaries and encoding are not enabled.
Decoded bodies are not accumulated in memory. Each JNI call copies bounded
regions rather than pinning/copying an arbitrary caller-sized Java array.

Native creation, streaming and destruction have a single owner. A cleanup-state
monitor serializes native calls against close/Cleaner retirement. Decoder state
is retired on success, failure and close. Cancellation closes the underlying
stream without waiting on the reader monitor, so a socket read blocked waiting
for peer data can wake. Decoder construction performs no network reads. The
first output from a flushed, unfinished Brotli stream is delivered while the
peer still withholds the rest of the stream.

The shared coding policy preserves explicit `Accept-Encoding`, uses `identity`
for range requests and advertises Brotli automatically only for HTTPS or a
localhost/loopback origin. Headers stay available before the response body is
read. HEAD and bodyless response statuses do not create a decoder. Recognized
coding chains are decoded in reverse order, with a ten-layer limit; an unknown
or identity coding preserves the complete body unchanged. Error response bodies
use the same real decoding path.

On decoder success, trailing encoded data is discarded while the raw HTTP body
is drained to its transport framing boundary. This preserves the provider's
cache commit/socket lifecycle rather than treating the end of one compressed
stream as transport EOF. Malformed/truncated responses fail and close acquired
resources. Raw-source close is idempotent across nested coding cleanup.

Decoded byte counts must not inflate network-body statistics. Unary and duplex
paths now account from encoded transport reads. Header-byte accounting is an
existing separate gap: this does **not** claim complete Cronet received-byte
metrics. Existing URL response headers are preserved.

## Source identities and Android/Chromium reference behavior

Base: `6afcb6e671fc4f2d08a4a98220f9d9a906a327bf`.

* AOSP Brotli, `android-16.0.0_r4`:
  [`39026f8dd0485dddcb0f9f5b12fb5ed788beca43`](https://android.googlesource.com/platform/external/brotli/+/39026f8dd0485dddcb0f9f5b12fb5ed788beca43/).
* AOSP OkHttp, the same release:
  [`e3d37d381375156dfad90887870775137e0481bf`](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/),
  reconstructed with the existing two repository flow-control patches. The
  existing test driver prints both patch identities and the provider tree hash.
* [Android `setEnableBrotli` contract](https://developer.android.com/reference/android/net/http/HttpEngine.Builder#setEnableBrotli(boolean)):
  opt-in behavior and disabled default.
* Chromium
  [`http_request_headers.cc`](https://chromium.googlesource.com/chromium/src/+/3e1fe2448617f75a6ebd5fa97a3d7d509da2bbfe/net/http/http_request_headers.cc):
  explicit-header, Range and secure/localhost negotiation policy.
* Chromium
  [`filter_source_stream.cc`](https://chromium.googlesource.com/chromium/src/+/3e1fe2448617f75a6ebd5fa97a3d7d509da2bbfe/net/filter/filter_source_stream.cc):
  unknown/identity chain passthrough, coding order and nesting limit.
* Chromium
  [`brotli_source_stream.cc`](https://chromium.googlesource.com/chromium/src/+/3e1fe2448617f75a6ebd5fa97a3d7d509da2bbfe/net/filter/brotli_source_stream.cc):
  incremental provider behavior and trailing-input discard.
* Chromium
  [`url_request_http_job.cc`](https://chromium.googlesource.com/chromium/src/+/3e1fe2448617f75a6ebd5fa97a3d7d509da2bbfe/net/url_request/url_request_http_job.cc):
  raw response headers and transport received-byte accounting.

The companion JSON records the exact hashes of these reviewed source files and
the production implementation. These are source comparisons; they do not
substitute for executing Android's HttpEngine on a device. Raw-deflate fallback,
new codecs and additional Cronet capability parity are outside this leaf.

## Executed validation

1. **10,881 native/HTTP assertions**, using the original production JNI bridge,
   the actual pinned native decoder and the actual patched OkHttp provider.
   Coverage includes input/output boundaries, empty/large bodies, every truncated
   prefix of the small fixture, malformed data, native retirement, true
   unfinished-stream output, blocked-reader cancellation, trailing input,
   coding chains, unknown encodings, negotiation, range/explicit headers, HEAD,
   204, error bodies, URL connections, cache hit/commit and malformed cache
   rejection. Native provider/JNI code runs with nonrecovering UBSan and the JVM
   with `-Xcheck:jni`.
2. Three additional real HTTP/2/native modes: a Brotli FLUSH/FINISH duplex
   exchange; headers delivered before a peer waits for the application's first
   write; cancellation with a reader waiting for encoded input and a writer
   blocked after exhausting 65,535 bytes of flow-control credit. The cancel
   test verifies one terminal callback and retirement of both workers.
3. The same public API wire workload against the complete **unchanged** base
   HTTP package and the candidate. The old package is compiled from Git; no new
   helper is injected into its implementation. Baseline requires zero Brotli
   responses; candidate requires ten and at least a 40% encoded-byte reduction.
4. The complete existing HTTP host suite passed after the final production
   edits: host/factory, flow control, transfer buffers, duplex/headers/cancel,
   H2 session reuse, TLS/proxy/session ownership, read-buffer work, private cache,
   connection lifetime and both TLS protocol-policy configurations. This
   includes 55 credit, 2,415 transfer, 150 session, 105 TLS/session, 122,513
   read-buffer, 8,420 cache and 167 connection-lifetime checks. Some concurrency
   check totals vary with callback scheduling.
5. Final app and services Java/D8 builds against the real main framework
   artifact and Android libcore module passed. The app build's boot symbol gate
   found 292 native references and no unresolved ones.
6. Production JNI plus all pinned decoder/common C sources compiled for both
   Windows x86-64 and ARM64, with actual patched AOSP nativehelper headers and
   `GESHER` enabled. This is cross-compilation evidence; runtime qualification
   is delegated to the full fork CI, including the Windows framework track.
7. Independent Java review replayed the 10,881 assertions and all three new H2
   modes. Independent native review rebuilt the JNI bridge, tested fourteen
   additional invalid/overflow/null/closed ranges and a valid decode under
   `-Xcheck:jni`/UBSan, and verified 262,140 unrelated destination bytes were
   untouched. Both reviews inspected the production ownership path.

The host JNI boundary adapts only built-in registration and the ABI to the host
JVM. It does not replace codec, network, cache or cancellation implementations.
The Windows track runs `android.net.http.HttpEngineBrotliTest` and the three H2
modes inside the actual runtime. The full CI also reconstructs the pinned native
provider and reproduces fixtures in `build-framework-assets`.

The reusable framework workflow's old detached-merge commit was not resolvable
by GitHub Actions. Its pin is moved to published base `6afcb6e`; the workflow
file has the **identical Git blob** `834692396e307a6cd9251c527b7a91ab402d13ac` at
both revisions. No workflow recipe, runner, permission or gate is weakened.
Workflow actor, release, cache and cache-pruning policy tests passed.

## Reproduction

Use JDK 21, GCC/G++, Python and the source checkouts fetched from `aosp.lock`:

```sh
python3 tools/test_http_brotli.py --java-home "$JAVA_HOME" \
  --brotli-source third_party/aosp/external/brotli \
  --okhttp-source third_party/aosp/external/okhttp \
  --output-dir /tmp/gesher-brotli-evidence

python3 tools/test_http_engine_policy.py --java-home "$JAVA_HOME" \
  --okhttp-source /tmp/gesher-brotli-evidence/okhttp-provider \
  --brotli-native /tmp/gesher-brotli-evidence \
  --source-ref 6afcb6e671fc4f2d08a4a98220f9d9a906a327bf \
  --brotli-wire-only --brotli-wire-baseline

python3 tools/test_http_engine_policy.py --java-home "$JAVA_HOME" \
  --okhttp-source /tmp/gesher-brotli-evidence/okhttp-provider \
  --brotli-native /tmp/gesher-brotli-evidence --brotli-wire-only

python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source third_party/aosp/external/okhttp
```

The first command requires a fresh output directory. The checked-in fixture is
verified on every run; `--write-fixtures` is reserved for an intentional fixture
regeneration. Raw measured lines and source hashes are in
`http-brotli-2026-11-23.json` beside this report.
