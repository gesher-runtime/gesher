# Private HttpEngine HTTP caching

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), N48/T12,
reserved in [the implementation scope](https://github.com/gesher-runtime/gesher/issues/56#issuecomment-7082880912).
This implements the engine's previously ignored HTTP cache preference. It does
not close the complete networking, Cronet, QUIC or request-metrics roadmap.

## Application contract and scope

An application can choose disabled, memory, disk without HTTP, or disk HTTP
caching through `HttpEngine.Builder`. Each built engine snapshots its builder
configuration. Memory caches are private to one engine. Disk HTTP caches persist
under the selected application storage directory and are exclusive to one live
engine in the process. Request-level cache disabling bypasses both lookup and
storage without destroying an already valid cached representation.

The Android API defines [cache modes and storage configuration](https://developer.android.com/reference/android/net/http/HttpEngine.Builder),
[engine shutdown](https://developer.android.com/reference/android/net/http/HttpEngine#shutdown()),
and [cached response and byte-count reporting](https://developer.android.com/reference/android/net/http/UrlResponseInfo).
The Android 16 r4 builder implementation is
[`CronetEngineBuilderImpl.java`](https://android.googlesource.com/platform/external/cronet/+/refs/tags/android-16.0.0_r4/stable/components/cronet/android/java/src/org/chromium/net/impl/CronetEngineBuilderImpl.java).
Its disk modes require a storage path; memory and disabled modes may have an
otherwise unused path. Only disk modes claim that path. Gesher follows those
rules, including release of ownership after shutdown and retention of the first
owner when a duplicate build fails.

HTTP freshness, validation, Vary selection, redirects, error-response caching,
mutation invalidation and incomplete-body handling are provided by the existing
pinned Android OkHttp implementation, not by a second cache-policy algorithm.
Reference checkout: external/okhttp `android-16.0.0_r4`, commit
`e3d37d381375156dfad90887870775137e0481bf`:

- [`Cache.java`](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okhttp/src/main/java/com/android/okhttp/Cache.java)
- [`DiskLruCache.java`](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okhttp/src/main/java/com/android/okhttp/internal/DiskLruCache.java)
- [`HttpEngine.java`](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okhttp/src/main/java/com/android/okhttp/internal/http/HttpEngine.java)
- [`HttpURLConnectionImpl.java`](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okhttp-urlconnection/src/main/java/com/android/okhttp/internal/huc/HttpURLConnectionImpl.java)

The change preserves the existing platform URL handler, TLS verification,
network binding, proxy selection and HTTP/2 policy. It attaches a private cache
to the connection's copied provider client using Android's `InternalCache`
bridge. It never replaces or installs a process-global `ResponseCache`.
Disabled engines also detach the inherited global adapter, so an application
calling `setUseCaches(true)` on the returned connection cannot opt that engine
into another cache. Unsupported private providers fail explicitly when caching
was requested; they do not silently pretend a private cache was installed.

## Storage and ownership

Requested size zero selects a 32 MiB implementation default. As in the API,
the configured HTTP cache size is advisory. Disk mode gives OkHttp a dedicated
`gesher-http-cache-v1` child directory: journal recovery must never clear the
application's entire storage directory. A failed initialization releases only
ownership that this construction actually acquired.

Memory mode supplies the same DiskLruCache policy with an in-memory FileSystem.
It performs no temporary-file I/O. Each file owns compact 8 KiB byte-array chunks;
tiny writes fill the current chunk rather than retaining a shared 8 KiB Okio
segment per incoming byte. Accounting charges allocated payload capacity,
including the last partial chunk, not merely logical data length. The backing
limit is twice the advisory size plus 1 MiB for journals and in-progress work,
with overflow-safe arithmetic. This is a bound on owned payload arrays, not a
claim about exact JVM RSS or all Java object/container overhead.

Unlink, truncate and replacement preserve a reader's immutable byte snapshot.
Old storage stays charged until its final reader/writer closes. An oversized
optional cache write fails through the provider's existing fault-hiding sink;
the application's successful network body still reaches it. Closing the backing
retires its namespace, and any retained raw snapshot releases its charged arrays
when its owner closes.

The engine remains active while either an asynchronous request's admitted
transport work or a public URLConnection's transport/body still owns resources.
EOF, close, failure, cancellation and disconnect retire the corresponding owner.
Unused URLConnection wrappers cannot start a request after engine shutdown.
Headers remain readable after a completed response. Local upload-protocol errors
retain a usable response, while HTTP errors retain their error body. The async
request owns every redirect hop until it is closed; cancellation cannot lose
the old hop merely because the next hop was queued. Terminal resource retirement
precedes release of the engine and delivery of its terminal callback.

`wasCached()` reads the actual provider response and its validation decision.
The provider's `CONDITIONAL_CACHE` header alone is insufficient: it also appears
when a conditional request obtains changed 200 data. Both 304 validation and
the pinned provider's older-Last-Modified validation are handled, while changed
network data is reported as uncached. A server-supplied cache-source header
cannot forge this result. Cached body bytes are excluded from the request's
network byte counter. The pre-existing broader counter remains incomplete for
headers, redirects and compressed network bytes; this change does not claim
complete wire-metrics parity.

The original negotiated protocol survives a cache hit and disk reopen. Android
Cronet persists ALPN in
[`HttpResponseInfo`](https://android.googlesource.com/platform/external/cronet/+/refs/tags/android-16.0.0_r4/stable/net/http/http_response_info.cc)
and retains the original response information when
[`HttpCache::Transaction`](https://android.googlesource.com/platform/external/cronet/+/refs/tags/android-16.0.0_r4/stable/net/http/http_cache_transaction.cc)
validates cached content. The pinned OkHttp cache's HTTP status-line encoding
alone loses the HTTP/2 protocol. The private adapter therefore stores a tagged
protocol field only in the cache's response copy, restores the actual provider
protocol on lookup and removes only its own final field before exposing the
response. Original server fields with the same name, including leniently parsed
wire values, remain intact. A corrupt or unknown private tag closes the snapshot
and becomes a cache miss; it cannot forge an application-visible protocol.

## Measured work reduction

`HttpEngineCacheTest` issues 65 sequential asynchronous reads of an identical,
fresh, cacheable 64 KiB resource. The independent loopback peer counts received
requests and body bytes it actually writes. Both modes use the same production
request, callback, provider and socket code. Every returned body is consumed.

| Configuration | Application reads | Peer requests | Peer body bytes | Cached responses |
| --- | ---: | ---: | ---: | ---: |
| Cache disabled | 65 | 65 | 4,259,840 | 0 |
| Private memory cache | 65 | 1 | 65,536 | 64 |

The enabled path eliminates **98.46% of these network requests and response-body
bytes**. This is a deterministic network-work result for repeated fresh content,
not an application-wide speedup, latency benchmark, or a claim about uncacheable
responses. One-shot and changed resources still require network work.

The exact historical module at main
`324dc7e41ea13ceedc3d52db3c8f7c2f3590e452` compiles against the same fixture and
provider. Its supposedly enabled mode still sends 65 requests and 4,259,840 body
bytes, reports zero cache hits and fails the new work gate. A no-op builder
implementation therefore cannot pass the improvement criterion.

## Validation paths

The shared cache fixture executes real production engine code, unchanged pinned
OkHttp Cache/DiskLruCache code and real HTTP sockets. It covers private-engine
isolation, global-cache isolation, fresh reuse, per-request bypass, no-store,
Vary and Vary-star, conditional 304, changed 200, older Last-Modified, POST
invalidation, cached redirects and errors, forged headers, truncation, 32
concurrent warm readers, disk reopen and path ownership, builder snapshotting,
unrelated application files, oversize writes, cancellation and the work gate.
Its explicit backing fixture tests 8,192 shared one-byte writes, retained-reader
replacement, snapshot bytes, saturated size arithmetic and final array release.
This internal fixture is described separately from the public HTTP tests.

`HttpEngineConnectionLifetimeTest` runs the actual provider and public connection
methods. `HttpEngineTlsPolicyHostTest` uses an independent TLS/ALPN wire peer
and an isolated generated trust store. It exercises private fresh cache reuse
through HTTPS with HTTP/1.1 and HTTP/2, both asynchronous and URLConnection
callers, alongside the existing global-factory, protocol and pooled-connection
isolation checks. Production certificate verification is unchanged.

Final local replay on 2026-11-22 passed 65 request contract checks, the separate
global-factory process, 2,415 transfer checks, nine actual-provider HTTP/2 wire
scenarios, **8,420 cache/transport/storage checks**, **167 connection/request
ownership checks**, 37/42/42 provider-policy checks, and 59 TLS/ALPN checks in
each of two global-factory modes. A second agent independently rebuilt and
replayed the final cache, ownership, policy and TLS fixtures with the same
results. The complete app and services fixtures compiled against the actual
framework and libcore classes and passed D8.

Final production source SHA-256 receipts:

| File under `app/java/android/net/http` | SHA-256 |
| --- | --- |
| `EngineHttpCache.java` | `1a0b7b8c6efb648869a68fdbb06cc175cbb1e10e7839d30690a87a46593b4482` |
| `HttpEngine.java` | `831df58a88a945c6950d8bb89e9dda3ed4e03496469e95ef7ee6c4ca3b969774` |
| `JavaHttpEngine.java` | `1937427db2f8022fa7b93f27cf992cc0e66e74263f64db6ee4af7b0ca9932982` |
| `JavaEngineConnection.java` | `864c86feb7505060ce24d87f10eac13e16cad7d1b9cf2b133465ba7257453fbf` |

`tools/test_http_engine_policy.py` is chained from the existing full HTTP helper,
which is already executed by the pinned networking builder. Both shared cache
and lifetime fixtures are compiled into `services-tests.dex` and run by Windows
track 94 against real ART/framework/provider classes. Host Context, Network,
socket tagging and platform policy are explicit boundaries. Host JVM execution
is not presented as Windows/ART, a device receipt or completed full fork CI.

## Reproduction

Use JDK 17+ and the exact pinned external/okhttp checkout:

```sh
python3 tools/test_http_engine.py \
  --java-home /path/to/jdk \
  --okhttp-source /path/to/android-16.0.0_r4-okhttp
python3 tools/test_http_engine_policy.py \
  --java-home /path/to/jdk \
  --okhttp-source /path/to/android-16.0.0_r4-okhttp --cache-only
# Expected failure: old production ignored the enabled-cache setting.
python3 tools/test_http_engine_policy.py \
  --java-home /path/to/jdk \
  --okhttp-source /path/to/android-16.0.0_r4-okhttp \
  --cache-workload --source-ref 324dc7e41ea13ceedc3d52db3c8f7c2f3590e452
```

The complete app and services fixtures must also build through `app/build.sh`
and `tests/dex-services/build.sh`, using actual framework classes, the libcore
module and D8. The Windows track provides downstream runtime qualification.
