# Bounded UrlRequest read storage

Issue [#56](https://github.com/gesher-runtime/gesher/issues/56), N48/T12,
[reserved scope](https://github.com/gesher-runtime/gesher/issues/56#issuecomment-7082880912).
This is the asynchronous `UrlRequest` body path. BidirectionalStream's separately
implemented HTTP/2 transfer path was optimized in #275. The lifecycle prerequisite
is [#282](https://github.com/gesher-runtime/gesher/pull/282).

## Operation and ownership

The old request reader allocated another byte array on every application read,
up to 32 KiB each time. The actual Android provider commonly returns a shorter
segment, so a 64 MiB body caused about 270 MiB of scratch-array allocation.

Each request now lazily owns one scratch array. Its capacity grows geometrically
when the caller requests more space, with the existing 32 KiB ceiling. A series
of gradually increasing caller ranges allocates less than 64 KiB cumulatively;
small fixed ranges retain their small buffer. Every underlying read still uses
the minimum of the caller's remaining range and 32 KiB, even when an older array
has more capacity. The response bytes and copies into the application's direct
buffer are unchanged.

The existing request state machine admits one read at a time. The current reader
finishes using its array before publishing READY or dispatching the callback.
A direct callback may schedule the next read after that point. Synchronization
and executor submission publish the previous capacity to that worker. Cancellation
does not free storage while an admitted worker can still use it. Terminal cleanup
sets the owned reference to null only after the worker count reaches zero, before
engine release and the terminal callback. Retaining a completed UrlRequest no
longer retains this payload array.

The Android contract requires a writable direct buffer, preserves its limit,
advances its position by delivered bytes and requires the next explicit read to
continue delivery. See [UrlRequest.read](https://developer.android.com/reference/android/net/http/UrlRequest#read(java.nio.ByteBuffer))
and [UrlRequest.Callback](https://developer.android.com/reference/android/net/http/UrlRequest.Callback).
No URL, content, request policy, callback ordering, permission or capability is
substituted. Per-request scratch storage is an internal resource change.

## Actual-provider measurement

The fixture executes unchanged production requests and pinned Android 16 r4
OkHttp at `e3d37d381375156dfad90887870775137e0481bf`. An independent loopback peer
serves the complete deterministic body. The application checks every byte,
buffer identity, position, limit and untouched prefix/suffix. A test-only stream
observer records the arrays actually passed to the real provider; all response
bytes still originate at the socket. Explicit short-read cases limit a delegate
read to 113 bytes; they are identified separately from the unmodified provider
workload. The observer retains only the latest array and does not modify any
production source.

HotSpot thread counters independently record allocated heap bytes and CPU time
for all `Gesher-HttpEngine` workers. They include ordinary task, callback-dispatch
and provider allocations, not only the read array. They exclude the independent
peer and application callback thread. They are not RSS or whole-process CPU.

The historical baseline is local commit
`59f16442fd47918b772dca3e12f4fa1a7be56e2f`, published as
`9740b6b93e3c6dc88ad49b5a6eac4f491a914799` with the identical tree
`e41c0506c5927e0cfb1267e22128e6a9eca898fe`. The helper compiles that entire module
unchanged against the same fixture and provider. Both variants warm up with a
1 MiB transfer before the measured 64 MiB transfer.

| Sequential run | Variant | Provider reads | Scratch array bytes | Worker heap bytes | Worker CPU ms | Elapsed ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | Candidate | 8,255 | 32,768 | 1,110,496 | 218.29 | 396.09 |
| 2 | Baseline | 8,227 | 269,582,336 | 270,783,408 | 277.92 | 773.86 |
| 3 | Baseline | 8,447 | 276,791,296 | 278,029,488 | 724.93 | 1,170.98 |
| 4 | Candidate | 8,435 | 32,768 | 1,132,504 | 205.73 | 364.21 |

All four deliver exactly 67,108,864 bytes with checksum 8,556,380,160. The candidate
uses one 32 KiB scratch array in each large transfer, eliminating over **99.98%**
of observed scratch-array bytes. Whole worker allocation falls over **99.5%**.
The 256 KiB mixed/short-read cases fall from about 41.45 MB to 41,225 scratch bytes;
the gradual-growth case falls from 1,095,353 to 2,047 bytes. Four simultaneous
1 MiB requests each own distinct storage and allocate 32,781 scratch bytes.

Socket segmentation makes individual read/check counts vary. CPU and elapsed
time also vary in this shared build environment: an earlier exploratory pair
had reversed elapsed-time order. The table is a resource measurement with timing
disclosed, not a stable throughput ratio or application-wide speed claim. No
timing threshold is used as a correctness gate. The baseline fails the actual
candidate resource gate with `Read scratch allocation grew with body/read count`.

The [machine-readable receipt](http-request-read-buffers-2026-11-22.json) records
all workloads, execution order, log hashes, provider identity and measured
production/fixture source hashes. Production `JavaHttpEngine.java` SHA-256:
`d9f9ca14489b25b3ad5a7b22e4abc001a9ce212641c0554ea4062e9ce1c09142`.

## Validation and boundaries

The shared fixture covers large/empty/truncated bodies, redirects, short reads,
gradual capacity growth, changing caller ranges, direct and asynchronous
callbacks, four concurrent requests, callback exceptions, cancellation before
the first read and cancellation while a real provider read still owns its array.
The cancellation gate proves that shutdown and terminal delivery remain blocked
until the admitted reader retires, and that canceled data never modifies the
application buffer. A separate internal assertion checks payload retirement while
the public request remains strongly reachable.

The complete existing HTTP helper passes its 65 request contracts, separate
global-factory process, 2,415 HTTP/2 transfer checks, nine HTTP/2 wire scenarios,
8,420 cache checks, 167 ownership checks, 37/42/42 protocol-policy checks and
59+59 TLS/ALPN checks. The full application and services fixtures compile against
actual framework/libcore classes and D8. Windows track 94 includes the shared
read fixture. Windows/ART execution and a complete published-head fork CI pass
remain pending; host native/platform adapters are explicit test boundaries.

An independent second agent reviewed the production ownership path and replayed
the actual-provider fixture normally and with HotSpot `-Xint`. The latter's
64 MiB transfer took 16.084 seconds under the existing 30-second request deadline.
Windows track 94 uses ART's interpreter, so its explicitly labeled large-body
resource gate transfers 2 MiB; host measurements retain 64 MiB. Both use the same
byte oracle, 32 KiB cap, mixed/growth/concurrent reads and cancellation tests.
The smaller target workload still rejects per-read allocation and does not
change any controller deadline or present host results as Windows execution.
The receipt's measurement fixture hashes identify the four recorded host runs
before this target-size-only fixture adjustment; production source is unchanged.

## Reproduction

```sh
python3 tools/test_http_engine_policy.py --java-home /path/to/jdk \
  --okhttp-source /path/to/pinned-okhttp --read-only
python3 tools/test_http_engine_policy.py --java-home /path/to/jdk \
  --okhttp-source /path/to/pinned-okhttp --read-only --read-baseline \
  --source-ref 9740b6b93e3c6dc88ad49b5a6eac4f491a914799
# Expected failure of the new resource gate on unchanged historical production:
python3 tools/test_http_engine_policy.py --java-home /path/to/jdk \
  --okhttp-source /path/to/pinned-okhttp --read-only \
  --source-ref 9740b6b93e3c6dc88ad49b5a6eac4f491a914799
```

Run `tools/test_http_engine.py` with the same dependency arguments for the full
HTTP regression set. This leaf does not close the broad N48/T12 roadmap rows.
