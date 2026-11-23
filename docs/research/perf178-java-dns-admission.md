# Java DNS admission and completion ownership

Refs #178, NATIVE-10 Java API subround. Base: 8b316aebf09ab46d5407e955bf86ed748a1e739f.

## Observable behavior and scope

The previous android.net.DnsResolver copied a raw query before checking cancellation and created a daemon thread for each query. A slow synchronous Windows lookup, or an application executor that retained completed callbacks, had no shared request or byte budget.

The complete production resolver now admits a request before copying its raw input, queues work on a bounded lookup lane, and uses a separate bounded lane to call application executors. Cancellation removes queued work. A query deadline can report a timeout while a Windows call remains active; its native owner stays charged until that call actually returns. Accepted callback wrappers remain charged until consumed, including after cancellation clears their bulky payload.

The caller still selects the network, query input and callback executor. This round preserves the existing A/AAAA lookup, address-family filtering, raw query ID and RCODE handling, malformed-input errors, null-argument exceptions, and cancellation suppression. The existing Java raw implementation synthesizes IN A/AAAA answers with TTL zero, returns NOTIMP for other types, and does not implement the API flags' cache/retry meanings. Those limitations are unchanged; this is not a DNS protocol rewrite or completion of all NATIVE-10 acceptance.

The native raw socket DNS owner is a separate workstream. This Java API uses WindowsNetworks.resolve, whose synchronous platform call has no cancellation handle exposed to this owner. The new Java limits do not pretend to forcibly terminate that call, bound every other DNS entry point, or establish a whole-process memory limit.

## Budgets

| Owned resource | Bound |
| --- | ---: |
| Accepted requests, from admission through actual lookup/execute/callback retirement | 256 |
| Simultaneous blocking lookup tasks | 32 |
| Simultaneous calls into application Executor.execute | 4 |
| Deadline worker | 1 |
| Raw input per request | 65,535 bytes |
| String input reservation per request | Twice the string length, at most 65,535 bytes |
| Input reservations across accepted requests | 8 MiB |
| Addresses accepted from one synchronous lookup | 4,096 |
| Encoded raw reply | 65,535 bytes |
| Retained completed answer reservations | 8 MiB |
| Query deadline, measured from admission | 30 seconds, monotonic |
| Retry after internal completion worker startup fails | One timer per affected request, at one-second intervals |
| Idle internal worker lifetime | 30 seconds |

The answer reservation is byte-array length plus 64 bytes, or 128 bytes per address plus 256 bytes for a list. It is a conservative logical payload allowance, not a heap profiler's object-size measurement. Up to 32 lookup producers can each hold one bounded result while waiting for the retained-answer budget; they remain counted as active lookup tasks. Their maximum list allowance is 524,544 bytes each. Temporary encoding buffers, platform resolver allocations, caller-owned callback graphs and allocator overhead are not included in the 8 MiB retained-answer counter. Raw reply size is checked before allocating its output buffer; oversized address arrays are rejected before constructing the output list.

Input reservations remain until the whole request retires, even when the raw bytes are no longer needed. This also conservatively retains credit for a query name carried by an address or error. Internal queues have capacity for the entire accepted set. Each accepted request has at most one queued completion. The pending timer queue is bounded by the accepted set, plus at most one successor briefly added before its predecessor is cancelled under the single owner lock. One timer action can also be executing. A startup retry does not add another completion wrapper. Cancelled timer entries are removed promptly.

## Ownership and failure decisions

Admission does not wait for capacity; signal registration retains CancellationSignal's existing synchronization behavior. An already cancelled request does not copy its input. Capacity exhaustion produces ERROR_SYSTEM with ErrnoException(EAGAIN); an oversized input produces EMSGSIZE. No request or copied query is captured by this rejection notification.

This follows the AOSP immediate failure path: the API caller invokes the supplied executor to report an error. That executor may run inline, block, reject, or retain the notification. Those rejected-call queues are application-owned and are outside the accepted resolver budgets. Claiming a global bound on an arbitrary application's error queue would require changing this delivery contract. The relevant primary implementation is [AOSP Connectivity DnsResolver](https://android.googlesource.com/platform/packages/modules/Connectivity/+/4416f38e4f624ed9562eb9ce7d33c4c0d5dc9bcf/core/java/android/net/DnsResolver.java).

For an accepted request, lookup, execute and callback ownership are independent. A callback that returns inline does not release an execute call which remains blocked afterward. Cancellation while a callback is running does not release the answer still held by that callback. A callback handed to an executor but never run retains its admission, even after cancellation; repeated cancellation cannot manufacture new capacity while old wrappers remain reachable.

The supplied executor runs outside the admission lock. A documented RejectedExecutionException means the executor did not accept the task, so its callback owner retires. An arbitrary exception can instead mean “enqueued, then threw”; in that ambiguous case the resolver clears payload but conservatively retains the wrapper's admission until it runs. If a nonconforming executor throws without ever retaining or running that wrapper, that slot can remain unavailable. Retiring it speculatively would allow an unbounded collection of retained wrappers. Duplicate runnable invocation is suppressed; callback and inline-executor exceptions do not double-retire owners.

The four completion workers keep a blocking application executor off the lookup lane. They cannot guarantee progress if every executor or callback blocks forever, or if callbacks synchronously wait for other callbacks on the same exhausted finite lane. The resolver does not start replacement lanes to hide that condition. Running native calls also remain counted after cancellation or timeout.

If starting an internal completion worker fails, the resolver keeps the original queued completion and its payload, then retries worker startup once per second on the existing timer worker. An already running dispatcher can consume that same queued completion. No application code runs on the timer or lookup lane as a recovery fallback. The current query deadline remains queued or running until a dispatcher exists or its retry is scheduled, so idle retirement cannot remove the timer needed to recover. Cancellation removes the queued completion and retry; a task already taken by a worker retains ownership until that worker observes cancellation. Repeated startup failure remains bounded by the accepted set. Once work retires, all three internal pools can again retire their idle workers.

ScheduledThreadPoolExecutor can enqueue a future before failing to start its first worker. The resolver records that actual future before insertion, then cancels and removes it if scheduling throws. Admission rolls back, preserving the original Error or the existing system-error path for a RuntimeException. It cannot accumulate unreachable timers outside the request budget. This handles worker-start failures; it does not promise that an exhausted Java heap can allocate new callbacks or retry objects indefinitely.

Cancellation listeners hold a weak reference to the signal rather than a request. A listener scans only the bounded accepted set and cancels every current request sharing that signal. Completion never clears a signal listener, which could erase a newer registration. Cancellation never calls setOnCancelListener(null) from inside its own listener, avoiding AOSP's cancellation-in-progress wait.

## Executable evidence

tools/test_dns_resolver_admission.py compiles the **entire** production DnsResolver.java. It runs the complete unchanged AOSP CancellationSignal from tag android-16.0.0_r4, SHA256 a671b35dfef0e319cc2855551d696aca845bc6ce25c59bb18867de5386048adc. [Original source](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/os/CancellationSignal.java) is included under its Apache 2.0 notice. The synchronous Windows resolver, Android value/errno types, annotations and unused cancellation Binder transport are explicit host boundaries. None is packaged into the runtime.

The suite covers 1/32/256/1024 bursts, accepted count and input credit, cancellation before copying, raw input mutation, queued and active cancellation, shared/reused signals and registration races, reply-byte pressure, executor-held answers, blocking execute, execute-after-callback blocking, rejection, enqueue-then-throw, duplicate callbacks, callback exceptions, reentrant submit and worker retirement. It uses synchronized snapshots under the actual production owner lock. Cancellation order may let another completed producer acquire newly released answer credit; the fixture checks actual retained wrappers rather than assuming a fixed ordering.

A separate JVM runs the real 30-second production deadline against 32 blocked uncancelable calls plus one queued query. All 33 receive one timeout; the queued query never reaches the backend, the 32 native owners remain counted, and releasing the backend restores capacity without late success callbacks.

Independent review of the first candidate found three blocking cases despite the initial passing suite: a delayed timer allowed an already expired queued query to enter the backend; a dispatcher thread-start failure stranded an accepted completion; and a timer thread-start failure left its already queued future unreachable after admission rolled back. The revised suite retains a second real 30-second probe with the actual timer worker deliberately occupied. It also injects 1,024 timer-start failures, verifies original-answer delivery after a dispatcher-start failure, verifies recovery with an existing dispatcher when pool growth fails, and cancels a pending retry. A short-idle probe forces completion startup to take longer than the timer idle timeout while new timer creation is unavailable; the original deadline keeps the existing timer available for recovery. Only the idle timeout of the real executor is shortened for that retirement probe; the two deadline probes use the unmodified production clock and 30-second deadline.

Public-API negative controls compile the complete baseline class. A 64-call blocked burst exposes its per-query worker growth. Repeating a pre-cancelled 16 MiB raw call exposes the baseline's 128 MiB of caller-thread cloning across eight calls; the new path allocates only small submission scaffolding. These checks do not merely assert that new private fields exist.

~~~sh
python3 tools/test_dns_resolver_admission.py --java-home /path/to/jdk21
python3 tools/test_dns_resolver_admission.py --java-home /path/to/jdk21 \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f --case burst
python3 tools/test_dns_resolver_admission.py --java-home /path/to/jdk21 \
  --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f --case copy
~~~

The two baseline commands fail their bounded-worker/pre-copy assertions as expected. The measured eight-call allocation totals were 134,218,048 bytes on the baseline and 192 bytes on the revised path. The revised local run passed 4,021 host owner checks, 88 real-deadline checks and 44 delayed-timer checks. The ordinary timeout fixture completed in 30.024 seconds with 32 native owners still counted until release. Check totals can vary with legitimate cancellation races. The recovery regression also rejects the unchanged first candidate at the orphaned scheduled-future assertion. The complete revised app compiled and dexed, and the complete services test suite compiled and dexed against those app classes.

The ordinary host suite is wired into the Linux artifact-preflight job and both Windows host-contract runners. The existing FrameworkFixesTest DNS smoke gate remains unchanged. A new DnsAdmissionTest on the same ART/Windows track uses the actual runtime resolver and Windows localhost lookup to retain 256 completions, verify overload and cancellation ownership, consume those wrappers and recover capacity. It also checks actual resolver worker counts. Its owner snapshots synchronize on the actual production lock, so an initial callback's return is not mistaken for complete executor retirement. This new ART test compiled locally but was not executed in this Linux executor.

Host tests do not substitute for actual Windows runtime execution, controlled real resolver outage/latency measurements, foreground p95/p99 or the issue's multi-application/soak gates. No DNS coalescing, native cancellation or application-specific successful result is introduced.
