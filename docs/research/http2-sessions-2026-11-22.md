# Owned HTTP/2 session reuse — 2026-11-22

## Scope and source identities

This is issue #56's bounded N48/T12 **bidirectional HTTP/2 session reuse** leaf.
It replaces a physical connection per BidirectionalStream with eligible sessions
owned by the actual JavaHttpEngine. It does not close the N48 roadmap row.

The unchanged baseline is main `20252fde18db387b4d99d99f7050d6336f58bf6e`.
The measured implementation and fixtures are commit
`9b2b7f08821a74346e6dce2da90babdfe2b1a35e`, tree
`49d56985f1e0a7d906d94779b0d55a15e3efb6a5`. Later documentation commits do not
change those measured production bytes. The accompanying
[machine-readable receipt](http2-sessions-2026-11-22.json) contains every ABBA
sample, production SHA256 and the compiled provider fingerprint.

The provider base is the real AOSP external/okhttp checkout at
`e3d37d381375156dfad90887870775137e0481bf`, selected by this repository's
`android-16.0.0_r4` lock. This PR also patches its actual receive-credit ownership:
shared sessions require discarded DATA to return connection credit. The sorted
path/content SHA256 for the compiled framing, Okio and direct dependencies is
`8184665f76910273531de3fe3427018f5954ec603ba968283368cf5917aa8174`.
The unchanged baseline provider fingerprint is
`40f70f9682883648792803e0bb969e26c1ec4687fcad095286ea3ee67e598391`.

The helper reconstructs the clean pinned provider from its Git archive and
applies exactly this revision's provider patches in a temporary directory. It
never mutates or inherits patches from the supplied checkout; already-patched
CI checkouts and unchanged historical controls therefore remain truthful.
`--source-ref` selects both the complete historical HTTP package and that
revision's provider patches. The tool emits the base, patch digest, compiled
fingerprint and every production HTTP source hash. The normal AOSP fetch/build
pipeline applies the same patch to the packaged core-libart provider.

Primary contracts inspected:

- [HttpEngine lifetime and network binding](https://android.googlesource.com/platform/external/cronet/+/81e20e97483640db82a485af0a56ef418d76b3ce/android/java/src/android/net/http/HttpEngine.java)
  at that fixed source identity: shutdown rejects active requests and may wait
  for resource cleanup; created requests retain their selected Network. This
  API source is a contract reference, not an Android runtime comparison.
- [RFC 9113 stream concurrency](https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1.2),
  [SETTINGS](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5) and
  [GOAWAY](https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8): stream limits
  are directional, zero is a real limit, the setting is unsigned, and GOAWAY
  stops new admission while accepted streams can finish.
- Pinned provider [FramedConnection](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okhttp/src/main/java/com/android/okhttp/internal/framed/FramedConnection.java),
  [FramedStream](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okhttp/src/main/java/com/android/okhttp/internal/framed/FramedStream.java)
  and [Okio socket adapters](https://android.googlesource.com/platform/external/okhttp/+/e3d37d381375156dfad90887870775137e0481bf/repackaged/okio/okio/src/main/java/com/android/okhttp/okio/Okio.java),
  inspected from the actual checkout. `newStream` serializes its frame writer;
  `closeLater(CANCEL)` removes one stream and wakes its flow-control waiters;
  shutdown is connection-wide. Socket Sink timeouts close stalled physical I/O.

No framing implementation, test response or application payload is substituted
in production. Reflection remains the existing boundary to core-libart's pinned
provider; the new pool also reads its actual shutdown state and stream limits.

## Ownership, routing and bounded idle work

A route consists of scheme, case-normalized host, effective port, the exact
supplied Network object, selected Proxy and exact default TLS socket-factory
object. Different engines never share a pool. Different origins never coalesce,
even when they resolve to the same address or use a certificate covering both.
Proxy selection and cleartext policy are evaluated again for every acquisition.
An application changing its TLS factory cannot borrow a session authenticated
by a different factory. Actual HTTPS hostname checking and h2 ALPN remain
required. HTTP CONNECT carries the selected origin; explicit Network sockets
and proxy-name resolution still use their supplied owner.

Each transport holds one session lease from acquisition until all admitted
transport operations retire. New connections are not reusable until the real
connection handshake and preface complete. Consequently simultaneous cold
requests can open more than one connection. A warm eligible session multiplexes
streams. The pool conservatively includes delayed/unstarted transport leases
when checking capacity; the final stream admission also checks the provider's
actual open-stream count and current unsigned limit.

Zero-capacity admission waits on the actual connection monitor, has a 30-second
deadline and wakes on cancellation. A maximum 100 ms recheck is necessary only
while admission is blocked: this provider does not notify that monitor for a
settings-only capacity change. Ready requests and idle pools do not poll.
Admission serialization ends before stream body I/O. Independent readers and
flow-controlled writers retain their private bounded buffers.

Canceling a started stream sends that stream's CANCEL reset; it does not close
the shared connection. Canceling a sole connection attempt closes its actual
socket, including a blocked CONNECT or TLS operation. A cancellation racing
stream creation captures the returned source and resets the new stream before
allowing any subsequent operation. The
transport clears its connection/source/sink references only after admitted
operations retire. JavaBidirectionalStream counts admitted I/O, queued serialized
writes and cleanup: the engine registration and terminal callback are released
only after those owners have retired. User callbacks retain their original
serial executor, byte counts and binary payload semantics.

GOAWAY marks the real provider connection ineligible for new acquisitions;
accepted peers drain on it and a subsequent request creates a replacement.
Broken physical connections still fail their dependent streams. There is no
automatic replay of an application upload or silent retry of a refused stream.

At most five idle sessions remain per engine, with a 60-second idle lifetime.
Excess idle owners retire oldest first. A one-shot scheduled expiry closes real
sockets outside the pool monitor. One process-wide daemon waits on its delay
queue; it does not use a core timeout that repeatedly polls a future deadline.
Canceled scheduled tasks are removed. Engine shutdown cancels its pending
expiry and closes every remaining owned physical connection. A live request or
unflushed delayed stream remains an active owner, not an idle eviction candidate.

The shared socket has no read-wide timeout that could kill healthy peers because
another stream was idle. Each stream has independent 30-second read/write
timeouts. Physical SocketSink writes also have a 30-second timeout: connection
prefaces, HEADERS and control frames run outside FramedStream's timeout. Without
this backstop, a peer that stopped reading could hold a HEADERS writer and its
engine registration indefinitely. That physical failure is correctly connection
wide. It does not weaken ordinary stream-local cancellation or flow control.

## Shared receive-credit ownership repair

Independent source review found a blocker before publication: the pinned
provider replenishes connection credit for application reads but loses credit
when a stream is canceled or closes with an unread body. It can also append
newly received bytes after a reset, discard late DATA without acknowledgment,
and omit HTTP/2 padding from accounting. A private connection hides these
losses when it closes; a reused connection can strand unrelated streams.

The provider patch accounts for each consumed or discarded byte exactly once.
Reset and source-close clear only the stream-locked read buffer. The receive
buffer remains owned by the reader; a read completing across reset discards its
own bytes after rechecking lifecycle state. Connection accounting occurs outside
the stream lock. Unknown-stream DATA, rejected push bodies and flow-control
rejections also return their consumed bytes. The actual frame parser accounts
for padding plus its length octet, while live streams replenish their separate
padding credit. Transport retirement explicitly closes the actual Source after
admitted I/O retires, including the both-FIN case which correctly preserves
unread data until the caller closes it.

`Http2CreditContracts` runs the real provider with an explicitly parked in-memory
Source and recording Sink, no network endpoint and at most **1,025 bytes per
DATA operation**. It checks cancellation, peer reset, repeated close, partial
reads, late DATA, another surviving stream, both-FIN unread retirement, padding,
rejected push, deterministic receive/retire races and the writer's actual
WINDOW_UPDATE encoding. All **51 checks pass**. The identical test compiles and
executes against the unchanged pinned provider, producing **24 expected
failures**. Internal reflection only reaches pre-existing members; the negative
control does not fail because a new API is missing. The same bounded test is
packaged in the services DEX and required by Windows track 94.

The independent reviewer traced all four provider files and all four pool,
transport, stream and engine files, including locks, cancellation publication,
queued worker ownership, idle cleanup and both-FIN retirement. Its source review
has no remaining actionable blocker. That is source review; the execution
results below are the author's local evidence.

## Actual work comparison

The same independent wire peer serves deterministic 4,093-byte binary bodies.
Both complete production packages run in fresh JVM processes with the same
JDK 21. The baseline uses its unchanged pinned provider; the candidate uses the
reviewed receive-credit patch on that same base. The measurement order is baseline/candidate/
candidate/baseline. Certificates and TLS sessions are real loopback fixtures;
the baseline is allowed normal TLS resumption. A completed TLS handshake is
counted at the server, not estimated from a connection counter.

| Sequential workload | Baseline physical connections | Candidate physical connections | Payload bytes in each run |
| --- | ---: | ---: | ---: |
| 64 HTTP/2 GETs | 64 | 1 | 261,952 |
| 32 TLS HTTP/2 GETs | 32 | 1 | 130,976 |

Both candidate samples remove **63/64 physical connection establishments
(98.4375%)** in the cleartext corpus and **31/32 actual TLS handshakes (96.875%)**
in the TLS corpus. Every body and received-byte count is checked. These are
work reductions, not modeled savings or a claim about total application RSS.

| Sample, in order | 64 GETs elapsed (ms) | 32 TLS GETs elapsed (ms) | TLS handshakes |
| --- | ---: | ---: | ---: |
| Baseline A1 | 758.674600 | 1,205.866997 | 32 |
| Candidate B1 | 186.862097 | 256.981299 | 1 |
| Candidate B2 | 199.726644 | 282.371942 | 1 |
| Baseline A2 | 584.171396 | 1,143.992918 | 32 |

Elapsed measurements include actual callbacks and body verification. This is a
shared executor, not an isolated performance machine; JIT and concurrent host
load visibly affect the results. Timing has no pass/fail ratio. The deterministic
acceptance budget is the actual connection/handshake count and correct results.
No Internet-latency, Windows throughput, energy or whole-application speedup is
inferred from this loopback experiment.

### Existing bulk transfer control and costs

The maintained production adapter control transfers 64 MiB in each direction
with actual AOSP Okio and explicit deterministic Source/Sink boundaries. It is
an in-process adapter control, not network throughput. The final repaired source
passes all 2,415 assertions and preserves checksum 528,384. It requests 901,368
allocation bytes for reads and 294,912 for writes, exactly the unchanged
baseline control's totals. The final replay observes 10.555331 ms read-thread CPU
and 4.138942 ms write-thread CPU; no CPU speed ratio is an acceptance claim.

The receipt retains the earlier pre-repair ABBA transfer/session samples under
an explicitly historical key. The main session table above is a fresh ABBA run
of the final repaired source. Timing varies with JIT and executor load. This
leaf's benefit is reduced connection setup with safe stream ownership. A pool
intentionally retains up to five idle sockets/provider reader threads for
60 seconds, whereas the original retired every completed physical connection;
that bounded retained resource cost is explicit.

## Qualification and negative controls

The shared public corpus, used by both the host JVM and the mandatory Windows
ART track, passes **124 candidate assertions**. It verifies 64 sequential
requests, then 16 held concurrent streams on the same warmed physical connection.
One application cancellation and one server RST terminate only their streams.
GOAWAY arrives while the other accepted streams remain active: a new request
opens one replacement, old peers still deliver exact bodies, and the drained old
connection closes. Shutdown with active requests is rejected; final shutdown
closes every peer socket and terminal callbacks remain exactly once.

The original public cancellation fixture still exhausts exactly 65,535 bytes
of HTTP/2 write credit and observes both actual provider reader/writer waits.
An additional GET completes on that same single accepted socket while the first
stream's two directions are blocked. Canceling the first stream retires both
workers and its scratch without changing the transmitted credit count. Physical
EOF is checked after engine shutdown, since a shared healthy connection remains
owned by the pool after stream cancellation.

The host-only session suite passes **105 assertions** covering real TLS
handshakes, certificate trust and hostname failures, missing h2 ALPN, two actual
CONNECT proxy routes, engine isolation, explicit Network owner identity and
builder capture, 0→1→uint32-max peer settings, delayed-header cancellation,
bounded idle eviction and an actual shortened idle-expiry control. Restoring the
same valid TLS factory reuses its eligible session after a failed untrusted
probe. Neither the untrusted nor wrong-host fixture sends an HTTP request.

A real CONNECT response is withheld, cancellation retires the connecting worker,
and no TLS or HTTP request reaches its destination. A separate real TCP peer
stops reading with small actual send/receive socket buffers. A large incompressible
HEADERS block reaches the production socket-write wait. The test first verifies
the production 30-second physical timeout, then uses a five-second isolated host
timeout; cancellation completes only after that admitted writer retires, and
engine shutdown succeeds. It does not fake a successful write or close unrelated
healthy streams to make the ordinary cancellation test pass.

Host boundaries are explicit: Context, Network namespace routing, Android policy
annotations and logger setup are fixtures. Network objects delegate to real
loopback socket creation and DNS lookup, with counters for the selected owner.
The certificate, hostname validation, ALPN, CONNECT bytes, independent frame peer,
binary bodies, actual provider, real socket blocking and resource retirement are
not modeled. Short expiry/write-deadline controls never change production defaults.

The complete unchanged baseline passes **119 shared functional assertions** and
32 TLS requests in its comparison mode. Running the candidate's one-connection
acceptance budget against that unchanged baseline fails on its real 64 physical
connections. An earlier comparison attempt exposed a test-peer cancellation
race: a closed private connection retained pending reply bookkeeping. The peer
now retires that bookkeeping with its actual socket, waits for baseline socket
retirement and preserves the original failure through cleanup. All final ABBA
runs pass with the same corrected fixture; no production baseline was edited.

The full production HTTP request/global-factory corpus, all nine original wire
modes, 2,415 adapter contracts, provider policy 37/42/42 and TLS policy 23/23 pass.
Final full app and services Java+DEX builds passed against the actual framework
classes and Android libcore module, with the 16 existing CompatChanges sources.
The independent source review and exact build/test log hashes are recorded in
the JSON receipt. Windows/ART execution is mandatory acceptance, not a local
claim.

## Reproduction and maintained gates

```sh
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source /path/to/pinned/external/okhttp
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source /path/to/pinned/external/okhttp --sessions-only
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source /path/to/pinned/external/okhttp --sessions-only \
  --http2-mode sessions-baseline --source-ref 20252fde18db387b4d99d99f7050d6336f58bf6e
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source /path/to/pinned/external/okhttp --transfer-only
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source /path/to/pinned/external/okhttp --credit-only
# Expected failure: original provider, same bounded in-memory contracts.
python3 tools/test_http_engine.py --java-home "$JAVA_HOME" \
  --okhttp-source /path/to/pinned/external/okhttp --credit-only \
  --source-ref 20252fde18db387b4d99d99f7050d6336f58bf6e
```

`tests/dex-services/build.sh` includes Http2SessionContracts and the bounded
Http2CreditContracts in the actual services DEX. Windows track 94 adds `sessions` to its existing mandatory 45-second mode
loop without changing any deadline or success/exit check. A separate 45-second
credit gate requires a zero exit and the exact 51-check success marker. Framework CI's existing
`test_network_builder_contracts.py` requires the pinned provider checkout and
invokes the full HTTP engine suite, including the new host TLS/ownership cases.
The fast artifact-preflight HTTP gate remains intact.

QUIC/H3, Brotli, migration/DNS tuning, public-key pinning and per-request traffic
accounting are not implemented by this leaf. URI requests/URLConnection keep
their separate existing provider path. Private cache #282 and request-read
buffer #286 are independently composable work, not prerequisites for this PR.
The native Android Cronet module is not introduced. Real Windows/ART provider
reflection, TLS and application behavior remain required CI evidence before a
runtime pass is reported.
