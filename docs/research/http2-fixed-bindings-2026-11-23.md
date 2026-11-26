# Fixed HTTP/2 provider bindings — 23 November 2026

## Scope and ownership

This is a T12/N48 and issue #178 JAVA-05 reflection-resolution leaf. Main already
contains transfer-buffer reuse (#275) and owned session reuse (#299). Those
changes are retained. The remaining adapter resolves eight transfer methods and
multiple classes/constructors for every stream, resolves connection methods at
admission, and resolves two fields and two UTF-8 methods for every response
header. These are invariant members of the pinned runtime provider.

`Http2Bindings` now publishes one complete immutable set of class-member
metadata and immutable protocol/error enums per defining framework class loader.
There are no keys based on origins, engines, apps, headers or requests, and no
request/response objects enter that set. Each transport still creates its own
separate read/write Okio buffers. Sessions, leases, sockets and policy answers
retain their existing owners. The exact route key, default TLS factory, proxy,
network identity, current cleartext policy,30-second deadlines, flow credit,
GOAWAY retry and cancellation rules are unchanged.

The defining loader resolves the fixed `com.android.okhttp` classes; no context
or app-provided loader is consulted. The pinned FramedConnection, FramedStream
and Header classes are final. Reflection through ByteString's base method still
uses virtual dispatch for SegmentedByteString. A failed binding construction is
not published and does not poison a static initializer. Only metadata is cached:
`NetworkSecurityPolicy.getInstance()` and its current answer are invoked on
every cleartext route construction.

The cache retains a fixed metadata set for the framework loader's lifetime.
Exact retained heap/private-commit bytes were not measured. The first-request
allocation costs below must not be relabeled as retained-cache size. Provider
revision changes still require signature/field verification and the existing
actual-provider gates.

## Complete wire measurement

Baseline is the entire unchanged HTTP module at public main
`5aa6b39027e9c38de2781ff9cda066418d25b1ba`. Each variant reconstructs its own
provider patches on pinned Android16 OkHttp
`e3d37d381375156dfad90887870775137e0481bf`; both compile to the same provider
source digest `433e26fc882fce6216eb0a2792dafc9cf2e4726e5ca76f330328ce246d027733`.
All production and fixture SHA-256 digests are included in the JSON.

Six ABBA rounds produce144 fresh-JVM samples:12 samples per variant/case.
Every request completes over one real HTTP/2 connection. The independent wire
peer decodes actual request HPACK and checks method, target, duplicate order and
per-request owner. Its response uses separately written HPACK literals, including
ordered duplicates and UTF-8 values. The client checks every header and every
byte, including a unique per-stream body pattern, complete framing and absence
of spurious RST_STREAM. Warm cases first complete exactly64 unmeasured streams;
they are not a claim about a fully warmed long-running application/JIT.

The timed region includes all measured requests, transport retirement, pool
shutdown and peer-thread joins. Calling-thread CPU/allocation includes the
fixture's reflection and comparisons. Process CPU includes peer, provider
threads, JIT and cleanup. These are Linux host-JVM component results, not actual
Windows/ART, whole-application latency, TLS setup or hardware release budgets.
Other agent builds/measurements were paused during the final run; the host was
not exclusive hardware. All samples, stdout receipts and stderr are retained.
No outlier is excluded.

All entries below are medians. CPU and elapsed are milliseconds; allocation is
bytes on the calling thread.

| Workload | Caller allocated B | Caller CPU ms | Whole-process CPU ms | Complete elapsed ms |
| --- | ---: | ---: | ---: | ---: |
| First request, no extra headers/empty body | 2,377,908 → 2,425,256 | 67.921 → 66.431 | 165.000 → 165.000 | 72.978 → 74.530 |
| First request,16 headers/4,093 B body | 2,278,936 → 2,317,544 | 73.090 → 64.072 | 155.000 → 145.000 | 78.962 → 68.916 |
| 512 requests,0 extra headers/empty body | 7,719,544 → 5,196,448 | 183.627 → 144.272 | 700.000 → 550.000 | 268.603 → 229.619 |
| 512 requests,16 headers/4,093 B body | 17,584,344 → 10,398,112 | 260.445 → 230.344 | 1,385.000 → 1,165.000 | 342.974 → 327.203 |
| 512 requests,64 headers/4,093 B body | 36,203,756 → 17,078,020 | 285.671 → 229.965 | 1,385.000 → 1,190.000 | 388.149 → 327.161 |
| 512 requests,256 headers/empty body | 114,712,036 → 43,061,260 | 358.278 → 221.772 | 1,245.000 → 880.000 | 513.777 → 335.198 |

At0/16/64/256 extra headers in the repeated workloads, caller allocated bytes
fall32.685%/40.867%/52.828%/62.461%; caller CPU falls21.432%/11.557%/19.500%/38.101%.
Whole-process CPU medians fall21.429%/15.884%/14.079%/29.317%.
Those ratios describe these exact completed-stream workloads and include the
independent peer/validation where stated.

### Cold costs and limits

The first minimal request allocates2,377,908→2,425,256 B (+1.991%). Its elapsed
median is72.978→74.530 ms (+2.126%), and its observed interpolated p95 is
122.537→178.337 ms. First typical-request allocation is2,278,936→2,317,544 B
(+1.694%); its elapsed median improves78.962→68.916 ms, but its p95 also worsens
97.318→113.774 ms. Both cold-tail costs are retained. Twelve samples/variant do
not establish a release percentile, and shared-host variability does not justify
calling the observed regressions zero. No startup-equivalence or universal
latency improvement is claimed. The design resolves its complete fixed binding
set on the first H2 transport; that front-loads work formerly spread across
stream/setup operations.

## Contract and negative controls

- The maintained full HTTP suite passed: ordinary/global handler paths; H2 receive
  credit, transfer, direct-buffer duplex, gzip/header ordering, large body,
  read-only buffer, disabled H2, cancellation, pooled sessions/GOAWAY, TLS/ALPN,
  proxy/network isolation, cache and connection ownership; existing
  read/work checks remain intact.
- The new actual-provider/wire corpus passes151,980 checks in ordinary loading
  and again through an isolated defining loader. The fixture uses explicit
  reflection across private runtime boundaries, so it does not assume that equal
  package names share ART's boot/test loader.
- The isolated loader denies the actual Source class twice. Both attempts retain
  the original ClassNotFoundException. Once available, the real provider and
  complete wire corpus recover. Three cleartext deny/allow transitions replace
  the explicit host policy instance and prove the cache does not freeze it or
  its answer. This policy setter is a declared host boundary; Windows still uses
  its real platform policy and the required production fixture.
- Two independently compiled production mutants fail their precise unchanged
  assertions: publishing a partially constructed binding set breaks failed-lookup
  retries; caching a policy instance accepts a subsequently denied route.
  Compilation failures, crashes and timeouts are not counted as detected mutants.
  Complete receipts are embedded in the JSON.
- Full current-main app/services Java+D8 build passes; boot linkage finds292
  references and0 unresolved.21 release-workflow,6 cache-policy and3 workflow
  actor-metadata checks pass. The normal host suite and required Windows track94
  both invoke the new wire fixture. Existing gates/timeouts are retained.
- Native-agent independent source review and fresh pinned-provider replay passed
  both151,980-check corpora, retry and live-policy controls. Java-agent independent
  audit recomputed every median/p95/min/max and checked all144 complete receipts,
  ABBA ordering and source hashes (1,677 assertions). The performance audit is not
  a separate timed run. Actual Windows/ART/full aggregate CI is pending submission.

## Reproduction

Use a JDK21 and an unchanged checkout of the pinned Android OkHttp source:

```sh
python3 tools/test_http_engine.py --java-home "$JDK" --okhttp-source "$OKHTTP"
python3 tools/test_http2_bindings_negative.py --java-home "$JDK" \
  --okhttp-source "$OKHTTP" --output negative.json
python3 tools/benchmark_http2_bindings.py --java-home "$JDK" \
  --okhttp-source "$OKHTTP" --baseline-ref 5aa6b39027e9c38de2781ff9cda066418d25b1ba \
  --rounds 6 --output measurements.json
```

`--bindings-only` selects the new ordinary/loader wire
corpus. Benchmark class directories are fresh, and a changed candidate source
hash aborts publication of results.

## CI reference composition

Main's new framework reference `fe42fc3d4854141df94cd1e05cfbd10c4a686315`
failed actual fork workflow startup on #352/#353: Actions could not find the
called workflow, before any test. This branch uses published main
`5aa6b39027e9c38de2781ff9cda066418d25b1ba`, whose framework.yml blob is exactly
the same new `6dc4cca565aaf15fdf518eee825b0e44765b275f`. This is a reference-only
repair; no recipe, runner, permission or source-acceptance gate is downgraded.
Public Git-object access is not substituted for a successful Actions run.
