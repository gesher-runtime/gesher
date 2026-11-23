# Bounded unary HTTP admission

This is a component leaf for #56 T12/N48 and #178 NATIVE-10. It bounds the
unary `UrlRequest` path and preserves real provider work. It does not complete
NATIVE-10's DNS/account/response-byte/deadline work, replace H2 duplex scheduling,
or qualify whole-application performance.

## Problem and ownership

Main `d38b5549dc3963f8d491e9205f9b2743bb6d3745` starts a cached-pool worker for
every unary request. A slow peer therefore gives a burst of 1,024 requests 1,024
blocked client workers and connections. Merely changing to a fixed executor
would leave its backlog unbounded and could put socket cleanup behind the
blocked operations that cleanup needs to cancel.

The new scheduler admits transport work before starting the real connection.
Limits are process-wide across engines, with a separate per-engine share:

| Resource | Per engine | Process |
| --- | ---: | ---: |
| Active unary request owners | 32 | 128 |
| Outstanding owners, including queued and retiring terminal handoffs | 1,024 | 4,096 |
| Conservative initial URL/header admission charge | 8 MiB | 32 MiB |
| I/O worker threads | Shared | 128 |
| Socket cleanup worker threads | Shared | 4 |
| Terminal executor handoff worker threads | Shared | 4 |

Within an engine, request priority is respected, with FIFO order at the same
priority. Ready engines rotate when a backend slot is returned. A queued request
reports `WAITING_FOR_AVAILABLE_SOCKET`; cancellation removes it before any
provider/socket work. An exhausted count or metadata budget reports an ordinary
`NetworkException.ERROR_OTHER` through the supplied callback executor. It never
fabricates a response, successful upload, or terminal callback after that
executor has rejected execution. An allowed direct executor can execute that
failure during `start()`; no hidden emergency thread or unbounded overflow queue
is created.

Every started owner stays registered with its engine through transport cleanup.
Cancel disconnects available transports on an independent cleanup lane. A
worker inside an external operation remains counted until it actually returns;
cancel is not forced termination. The active permit retires only after all
request workers leave and owned transports/bodies have been closed. Initial
metadata/outstanding charge retires when terminal delivery starts, or when the
application executor rejects that delivery. An application executor that blocks
inside `execute()` cannot occupy the socket-cleanup lane.

Serial callback rejection needs both immediate and delayed handling. A terminal
handoff can be queued behind an earlier status/response callback, so its original
`execute()` caller may already have returned when the next scheduling attempt
fails. The serial executor now notifies all discarded owned tasks: terminal
tasks release their charge, nonterminal callback tasks fail their request, and
upload tasks settle their pending future. The independent reviewer reproduced a
retained charge of 1 before this fix and 0 afterward with the unchanged probe;
no impossible callback was synthesized.

Workers grow with queued demand and park/retire after 60 seconds without work.
Notification of one idle worker does not prevent additional workers from
starting before that notified worker reacquires the queue lock. Retirement and
the worker-count decrement occur under the same queue lock, preventing an
enqueue from being stranded behind the last exiting worker. No request, network
operation, application executor or application callback runs under the global
admission lock.

The 32-active-engine limit is **not** a claim of at most 32 live worker threads.
A retiring worker can briefly overlap its next scheduled read, and idle workers
from earlier activity remain parked. The three unary pools together have a hard
136-thread ceiling. Existing H2/URLConnection work and application-owned
executors are outside that ceiling.

## Complete-work comparison

`tools/benchmark_http_admission.py` compiles the entire unchanged HTTP module at
`d38b5549` and the candidate module, then runs fresh JVMs in six ABBA rounds for
each workload: 12 samples per variant per size, 96 complete samples per run.
Both variants use the actual pinned Android OkHttp provider
`e3d37d381375156dfad90887870775137e0481bf`, real loopback sockets and one selector
peer thread. Every request must receive and compare every byte of its 4,096-byte
body. Failed/incomplete/timeout executions cannot become timing samples.

The peer deliberately withholds headers for 200 ms after the permitted initial
connections reach it. Whole-work time and CPU include request construction,
admission, all connection setup, callbacks, body work and this fixed hold.
Post-release drain time is reported separately: baseline has already opened
every connection at that point, while the candidate must still connect queued
requests. This distinction prevents presenting unequal connection work as an
equal CPU comparison.

The following medians are from the final quiet run. Other local build lanes
paused heavy compilation/benchmarks during it; this is not exclusive hardware.

| Requests | Blocked client workers / connections, baseline → candidate | Complete bytes in each sample | Whole JVM CPU, ms | Whole elapsed, ms, including fixed hold | Blocked JVM RSS, MiB |
| ---: | --- | ---: | --- | --- | --- |
| 1 | 1 → 1 | 4,096 | 465 → 470 | 419.067 → 437.527 | 64.250 → 64.455 |
| 32 | 32 → 32 | 131,072 | 715 → 755 | 447.213 → 459.252 | 74.789 → 75.152 |
| 256 | 256 → 32 | 1,048,576 | 1,685 → 1,320 | 619.007 → 556.730 | 114.947 → 79.943 |
| 1,024 | 1,024 → 32 | 4,194,304 | 3,825 → 2,675 | 969.300 → 760.994 | 258.160 → 88.502 |

At 256/1,024 requests, the actual blocked worker/connection reduction is
87.5%/96.875%. Whole-component CPU is 21.66%/30.07% lower and elapsed time
10.06%/21.49% lower. The RSS reductions are 30.45%/65.72%, **including the host VM,
test callbacks, selector peer and provider**. They are neither pure-client heap
figures nor Windows private-commit or whole-process-tree claims.

### Latency and small-workload costs

| Requests | Median drain after peer release, ms, baseline → candidate | p95 drain, ms, baseline → candidate |
| ---: | --- | --- |
| 1 | 8.285 → 10.355 | 14.627 → 17.916 |
| 32 | 16.392 → 18.761 | 23.156 → 28.146 |
| 256 | 58.144 → 101.013 | 91.079 → 167.926 |
| 1,024 | 173.126 → 309.342 | 222.321 → 454.096 |

The admission limit trades later queued connection starts for bounded resource
use. The additional cleanup/completion handoffs also add about 2.1–2.4 ms to
median drain in the unsaturated 1/32-request cases. They remain independent to
preserve cancellation progress when application execution blocks. This is not a
claim of faster post-release draining.

Cold one-request/32-request whole elapsed medians increase 4.40%/2.69%. The
32-request CPU median increases 5.59%, which must not be omitted. Comparing each
ABBA round's two-sample means gives +2.84% CPU at 32 requests; a deterministic
20,000-resample bootstrap across only six round pairs spans approximately
−2.98% to +8.27%. This small host sample does not establish absence of a CPU
regression. The resource and burst-work gains are the claim; these ordinary-load
costs require review alongside them.

The explicit component tradeoff proposed for review is no more than 5 ms added
median post-release handoff time at 1/32 requests, and no more than 500 ms p95
post-release completion for this exact 1,024 × 4 KiB loopback fixture. This is a
fixture budget, not an Android timeout or production total-request deadline.
The quiet samples meet it. The retained run with concurrent local builds has a
2.449-second one-request drain outlier and does **not** meet it. Hardware/app
release budgets in #178 PERF-00/T12 remain unqualified; this PR must not be used
to claim that all latency or startup budgets pass.

Both complete final-source runs are retained in the JSON report, including that
adverse outlier, rather than filtering it from the data. Their production and
admission-fixture hashes match. Earlier unsubmitted prototypes are not used as
current-head performance evidence.

## Correctness and negative controls

The maintained fixture runs through public requests and the actual pinned HTTP
provider. Reflection observes private accounting or controls a documented
test-only return/lock boundary; it does not substitute the transport or decode
bytes. Its 13,582 checks cover:

- Complete 1/32/256/1,024 bursts, 1,024-engine and 4,096-process outstanding
  limits, initial metadata rejection, queued status and active-engine shutdown.
- Queued cancellation without peer/provider work, queue holes, provider closure,
  round-robin progress across five engines and highest-before-idle priority.
- Deterministic cancellation after engine registration but before submission.
  A separately held actual provider read retains its active charge until return.
- Four blocking allowed-direct terminal callbacks while 32 actual sockets are
  canceled and retired independently; rejected, throwing and reentrant callback
  executors; exactly-once terminal results where execution remains available.
- Four delayed serial rejection cases: response/terminal, each rejected
  directly or discarded behind another rejected status task.
- Worker demand behind an already-notified idle worker, queue capacity,
  idle retirement and 20 subsequent restarts.

`tools/test_http_admission_negative.py` compiles four isolated production
mutants and invokes the unchanged real-provider fixture. Each must fail its
specific assertion, not merely crash or time out: lost worker demand, completion
blocking cancellation, premature release of a live backend slot, and dropped
terminal ownership after delayed rejection. All four controls fail the intended
assertion. The JSON preserves their actual diagnostics and source hashes.

The existing full HTTP corpus passes: host callbacks/factory, H2 duplex, gzip,
trailers, disabled mode, credit accounting, transfer/sessions, actual TLS/proxy
ownership, cache, redirects/uploads/lifetime, protocol isolation and ALPN.
Read-buffer instrumentation was updated to observe all three new unary worker
names as well as the historical pool. A missing worker or a retired/unavailable
counter cannot be reported as zero work. Its corrected read suite passes
122,433 checks and measures 1,115,600 allocated worker bytes and 319,164,569 ns
worker CPU for one 64 MiB local run; no read-allocation improvement is claimed.
Full app and services Java/D8 builds pass, including 292 framework-to-app boot
references with none unresolved.

The default host HTTP gate now runs admission contracts. The normal services
DEX and required Windows track 94 contain the same public fixture, with a
180-second process watchdog. The outer host driver watchdog allows for the
additional suite; existing individual contracts and assertions are retained.
Full fork `ci.yml` will qualify the published head separately. Local JVM/D8
results are not Windows/ART execution.

## Scope boundaries and references

The metadata number is a conservative admission **charge** for the initial URL,
agent and header strings/entries, not a measurement of retained heap. It excludes
subsequent response/redirect headers, caller-owned upload/output buffers,
provider/cache/decoder storage and objects built but never started. Unary read
scratch retains the existing 32 KiB per-request maximum. The change does not cap
valid response length or invent a successful truncated body.

Existing 30-second connection/read/upload waits are preserved. No new queue
deadline is invented: applications may intentionally hold a read/redirect or
their callback executor. Such requests remain bounded and accounted until they
continue, cancel, fail or actually retire. Valid queued work advances when a
backend slot returns. Provider body-byte aggregation, total deadlines and the
separate H2/URLConnection/DNS/account paths remain further NATIVE-10 work.

Android contracts: [UrlRequest](https://developer.android.com/reference/android/net/http/UrlRequest),
[priority/direct-executor builder](https://developer.android.com/reference/android/net/http/UrlRequest.Builder),
and the pinned [AOSP JavaUrlRequest callback implementation](https://android.googlesource.com/platform/external/cronet/+/e05501550b3b70236b6c53f77befe65e1f87020f/components/cronet/android/java/src/org/chromium/net/impl/JavaUrlRequest.java).
Repository [scope policy](../../CONTRIBUTING.md) and
[#178 NATIVE-10](https://github.com/gesher-runtime/gesher/issues/178) retain their
broader runtime/hardware requirements. The CI framework pin uses published
`6afcb6e` instead of unresolvable `06d175a8`, the same repair as #346; both contain
framework workflow blob `834692396e307a6cd9251c527b7a91ab402d13ac`. No runner,
permission, source authority or acceptance gate is weakened.

## Reproduce

Use JDK 21 and the project's prepared pinned OkHttp checkout. Substitute local
paths for `$JDK` and `$OKHTTP`:

```sh
python3 tools/test_http_engine_policy.py --java-home "$JDK" --okhttp-source "$OKHTTP" --admission-only
python3 tools/test_http_admission_negative.py --java-home "$JDK" --okhttp-source "$OKHTTP" --output negative.json
python3 tools/benchmark_http_admission.py --java-home "$JDK" --okhttp-source "$OKHTTP" --baseline-ref d38b5549dc3963f8d491e9205f9b2743bb6d3745 --rounds 6 --output abba.json
python3 tools/test_http_engine.py --java-home "$JDK" --okhttp-source /path/to/pristine/pinned/okhttp
```

`--source-ref` compiles every production HTTP source from the named revision;
it does not run the old code through a candidate-only shortcut. Fresh JVMs keep
global URL handler and worker state independent between compared samples. The
raw report is [http-admission-2026-11-23.json](http-admission-2026-11-23.json).
