# HTTP/2 request-owned transfer storage

Baseline: `751d82310f7c4c470c098ccebfd5d2d04699a0cd`.
Refs #56 (N48/T12) and #178 (JAVA-05, partial).

## Production change and ownership

The old `Http2Transport` allocates a new Okio Buffer for every read/write,
resolves the same reflective methods repeatedly and returns a newly allocated
array for each read. Compressed input creates another temporary array before
copying to the decompressor's requested destination. `JavaBidirectionalStream`
also allocates an upload array for every chunk.

The adapter now resolves its fixed transfer methods once per transport and uses
one independent Okio Buffer per direction. Reads fill the body pump's reusable
32 KiB array, or the decompressor's own destination. The serial writer grows its
scratch geometrically, starting at the first required capacity (at least 256
bytes), up to 32 KiB. Small uploads do not immediately retain 32 KiB. Total array
allocation during writer growth is bounded by less than twice that limit.

The reader drains every byte admitted by the Source, including reads spanning
multiple Okio segments. Successful AOSP Source/Sink operations consume their
entire admitted buffer; exceptions clear only that direction's scratch and keep
the primary exception, suppressing any cleanup failure. Close does not mutate
buffers owned by in-flight operations. Terminal stream publication drops the
writer's retained array reference; an active write retains its local reference
until it returns. The body pump releases its local array when it exits.

Only the body pump reads, and only the existing serial writer writes. Application
callbacks still receive their own direct ByteBuffers. Scratch is never exposed
to callbacks, reused before its contents are consumed, or shared between duplex
directions. The existing one-chunk read-ahead, compressed-header callback order,
TLS/proxy/network binding, HTTP/2 policy and stream completion remain intact.

This implements the buffer/reflection part of JAVA-05. It does not change
connection ownership or add pooling/multiplexing. Existing framing/compressor
buffers, application-owned buffers and engine worker/admission policy remain.

## Measurements

Five alternating A/B pairs use a fresh JVM for each revision. Each JVM warms the
actual production adapter with 64 MiB, then measures another 64 MiB. AOSP Okio
sources are the unchanged pinned Android 16 r4 checkout at
`e3d37d381375156dfad90887870775137e0481bf`.

The controlled Source supplies real Okio segments; the Sink consumes them. This
isolates adapter work from network/TLS scheduling. Both revisions transfer the
same bytes and retain equal checksums. The baseline read output is observed
directly, without adding a copy into the candidate's scratch. Shared fixture
reflection is present in both measurements; there is no substituted production
read, conversion or write algorithm. Construction/warmup are outside the timed
range. ThreadMXBean reports allocations and calling-thread CPU; resident memory,
other engine threads, socket I/O and application throughput are not measured.
The timed checksum samples block endpoints; full-byte correctness comes from
the separate binary/decompression cases and the large wire echo fixture.

| 64 MiB operation | Baseline allocated bytes | Candidate allocated bytes | Reduction |
|---|---:|---:|---:|
| Read | 68,488,152 | 901,368 | 98.6839% |
| Write | 1,261,568 | 294,912 | 76.6234% |

Allocation counts are identical across all five samples. Median read CPU is
17.320 ms before and 9.370 ms after. Write CPU is effectively unchanged: 4.422 ms
before and 4.437 ms after. Elapsed medians are 22.726/9.363 ms for reads and
4.416/4.431 ms for writes. These short shared-host timings are informational,
not statistical Windows or application-wide speedup claims. In particular, the
write allocation reduction does not establish a write CPU improvement.

The write measurement deliberately excludes the additional per-chunk arrays
formerly allocated by `JavaBidirectionalStream`. Their removal is covered by
source ownership review and the actual wire tests, not counted in this table.
Read scratch is now retained for the pump's lifetime, trading bounded per-stream
storage for elimination of repeated allocation; required final application
buffers and framing memory are still present.

All repetitions, JVM/platform details and production hashes are in
[`http2-transfer-buffers-2026-11-22.json`](http2-transfer-buffers-2026-11-22.json).

## Validation and reproduction

```sh
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source third_party/aosp/external/okhttp

# Component-only baseline/candidate pair; alternate order for repeated pairs.
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source third_party/aosp/external/okhttp --transfer-only \
  --source-ref 751d82310f7c4c470c098ccebfd5d2d04699a0cd
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source third_party/aosp/external/okhttp --transfer-only
```

- 2,415 candidate component checks and 2,410 baseline checks: binary data, offsets,
  segment boundaries, EOF, gzip/deflate, truncated compression, partial source
  and sink failure, primary exception identity, cleanup and simultaneous duplex
  ownership. The candidate's five extra checks cover its new buffer API/bounds
  and allocation ceilings. Read and write ceilings are 2 MiB and 1 MiB,
  respectively, for the measured 64 MiB transfer, so both distinguish the
  original allocation behavior while leaving headroom above the candidate.
- Actual HTTP sockets: 65 HTTP engine checks and global factory behavior; nine
  HTTP/2 modes covering duplex, unary/trailers, gzip, headers-before-body,
  early reads, read-only uploads, a 1 MiB+37-byte upload with 4,093-byte reads,
  flow control, blocked duplex cancellation/scratch retirement and opt-out.
  Cancellation first receives all 65,535 bytes of initial write credit and
  observes separate reader/writer threads waiting in actual AOSP framing.
  It then requires socket closure, retirement of both observed I/O tasks,
  a barrier in the production serial callback queue, one terminal callback
  and no separately recorded late unexpected callback.
- Actual pinned-provider protocol cases: 37/42/42 checks. TLS/ALPN/provider-pool
  isolation: 23 checks in each global policy mode.
- Full application Java/DEX and services-test Java/DEX compile against the actual
  Android 16 core/framework inputs. No HotSpot-only API enters production or the
  Android wire fixture; allocation counters stay in the host-only fixture.
- Independent production review and component replay found no blocking scope or
  correctness issue. Source hashes bind that review to the measured production.
- The existing pinned framework recipe already calls the candidate's network
  builder helper. Its full run now executes this complete HTTP suite, preserving
  the prior policy checks. Windows track 94 also runs the new large/cancel modes
  using its existing compiled `Http2DeviceSmoke` class and normal failure gate.

Full Windows/ART target results, physical network performance, complete-engine
memory and the remaining JAVA-05/session/soak acceptance are separate from these
host observations. No such execution is claimed by the local measurements.
