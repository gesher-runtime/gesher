# Issue #178 NATIVE-10: bounded raw NDK DNS ownership

This round covers `android_res_nsend` / `android_res_nquery` through
`compat/posix/src/network_dns.cc`. It is one part of
[NATIVE-10](https://github.com/gesher-runtime/gesher/issues/178): Java
`DnsResolver`, HTTP engines, WebView and account dispatch remain separate work.

## Contract and scope

The [Android NDK contract](https://developer.android.com/ndk/reference/group/networking)
returns a pollable descriptor or an immediate negative POSIX error. Reading a
result closes that descriptor; cancellation attempts to stop an in-progress
query. This change keeps the caller's packet, ID, reply bytes, RCODE, selected
interface, server order, retry flags and UDP-to-TCP fallback. It adds no cache or
coalescing and does not change the shared Winsock/socket implementation, host
filtering, Java resolver or proxy behavior.

Previously each request allocated a socketpair, copied the packet and created a
detached Windows thread. Completed but unread answers remained registered without
an admission limit. Active cancellation correctly left close ownership with the
worker, but there was no overall budget for those still-running requests.

The new atomic reservation precedes server discovery, packet allocation, socket
creation and dispatch. Saturation returns `-EAGAIN`, using the existing native
immediate-error convention. The reservation is owned by the request object until
both the caller and worker retire. This includes canceled calls whose external
I/O has not returned and completed replies that callers have not consumed.

## Explicit resource policy

| Resource per process | Ceiling | Basis |
| --- | ---: | --- |
| Active DNS worker callbacks | 32 | The issue's first concurrent workload can run without a smaller admission width. |
| All admitted requests, including queued and unread replies | 256 | Admit the issue's 256-request stress tier and reject excess in the 1024 burst. |
| Scheduled or active Windows callback tickets | 32 | Cancellation and readmission cannot accumulate hidden callback objects in the Windows pool. |
| Query bytes per request | 65,535 | Existing raw DNS input validation. |
| Reply bytes per request | 65,535 | Existing DNS UDP/TCP length bound. |
| Request-owned descriptors | 544 | Two completion descriptors for each of 256 requests, plus one live UDP or TCP socket for each of 32 workers. |
| Retained packet plus reply payload, conservative bound | 33,553,920 bytes | `256 * (65535 + 65535)`; allocator metadata, socket buffers and thread stacks are additional bounded resources. |

These are named admission policies, not a claim that 32 is optimal on every
machine or that all Windows memory fits inside the payload bound. The owned
Windows pool has maximum 32, minimum zero and a blocking-callback hint. Idle
thread retirement belongs to Windows; there is no application polling thread or
periodic queue scan. See Microsoft's
[maximum](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-setthreadpoolthreadmaximum),
[minimum](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-setthreadpoolthreadminimum),
and [blocking callback](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadpoolcallbackrunslong)
contracts. The pool/environment/work object live for the process lifetime; they
are not reconstructed per request. Setup failure closes partially created pool
resources and permits a later retry.

## Queue, completion and cancellation

The FIFO contains at most one job per admitted request. A callback drains jobs
until the FIFO is empty. At most 32 callback tickets can be queued or running,
even if many queued requests are canceled and immediately replaced. The queue
mutex is released before resolver I/O or completion publication.

Cancellation first withdraws the caller descriptor from the registry. If the job
is still queued, it is unlinked and its writer is closed immediately. If a worker
already owns it, cancellation marks the request and shuts down the active socket;
the worker alone closes that socket and its writer. An external call that does
not return immediately retains its reservation. Cancellation never promises
forced thread termination or treats an in-progress call as free capacity.

Completion writes one byte to the socketpair, while the bounded raw reply remains
in the request object. There is no user callback under a runtime lock and no
completion callback queued behind blocking DNS work. Callers continue to use
poll/epoll and `android_res_nresult` as before. A canceled UDP receive returning
zero is checked immediately, avoiding a tight receive loop until timeout.

## One monotonic deadline

The existing per-server timeout calculation, two default retry rounds, three
maximum servers and one possible TCP fallback per exchange define the maximum
request budget. It is at most 60 seconds; `NO_RETRY` with one server has at most
10 seconds including a possible TCP fallback. The budget begins at submission,
so queued work cannot acquire a fresh budget when it eventually starts.

Each UDP or TCP exchange still receives its per-server timeout, clipped to the
remaining request deadline. Every partial TCP transfer recomputes the remaining
time, so a trickling peer cannot renew a socket timeout indefinitely. A slow
first server retains the established later-server and TCP-fallback opportunity.
Expired queued work completes with `-ETIMEDOUT` without opening a network socket.
Uninterruptible external work remains bounded and accounted for until it returns.

Socket/interface setup and nonblocking connect can consume this budget too.
The absolute deadline is checked after setup, immediately before connect/send,
and before each receive. TCP readiness polling receives the time remaining after
connect returns. Independent QA reproduced a late UDP packet when the previous
candidate used a timeout captured before delayed socket setup; the corrected
owner retires the descriptor without beginning expired network work.

## Measured source-owner evidence

`tools/test_dns_admission.py` compiles the complete production source bodies;
only include lines are removed. Its explicit boundaries supply interface/server
discovery, POSIX descriptor translation and, on Linux only, a thread-pool adapter.
Both hosts exchange actual local UDP and TCP packets. On Windows the fixture uses
the real private Windows thread pool and Winsock. A separate cross-compilation
compiles the untouched production translation unit against Gesher's POSIX headers.

The following measurements are **Linux production-owner evidence**, not Windows
runtime measurements. The baseline source is main
`8b316aebf09ab46d5407e955bf86ed748a1e739f`.

| Submitted burst | Baseline active / descriptors | Candidate admitted / rejected | Candidate active / descriptors |
| ---: | ---: | ---: | ---: |
| 1 | 1 / 3 | 1 / 0 | 1 / 3 |
| 32 | 32 / 96 | 32 / 0 | 32 / 96 |
| 256 | 256 / 768 | 256 / 0 | 32 / 544 |
| 1024 | Not run against the unbounded baseline | 256 / 768 | 32 / 544 |

An additional 256 completed, unconsumed maximum-size TCP replies retained exactly
16,776,960 answer bytes and continued to reject excess work until consumption.
All admitted results remained readable. The blocked-backend cancellation fixture
held 32 running calls after cancellation and performed 1024 queued cancel/readmit
cycles without adding callback tickets or freeing those 32 active reservations.

Changed-path checks also cover permission/argument/server/socket/pool failures,
queued input immutability and captured default-network identity, raw IDs and
non-address question types, maximum TCP replies, mismatched IDs, queue deadlines,
TCP trickling, and a real five-second unavailable first server followed by a
successful backup and TCP response. Delayed socket creation, TCP mode setup and
connect edges verify that expired setup cannot start connect/send or reset the
TCP polling budget. The final Linux run passed 2,922 assertions, and the new
setup-deadline regression rejects the previous candidate's stale timeout.
Existing exact-source lifecycle tests retain
their 188 ownership/app checks and both close-omission negative controls.

Local commands:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_dns_admission.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_dns_admission.py --source-ref 8b316aebf09ab46d5407e955bf86ed748a1e739f --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_dns_query_lifecycle.py --sanitize
python3 .github/scripts/test_automerge_workflows.py
```

The local executor's tracing prevents LeakSanitizer from running; only leak
detection is disabled above. AddressSanitizer and UndefinedBehaviorSanitizer
remain enabled. CI uses the ordinary sanitizer command without that local
override. The new Windows fixture is required on both Windows 2022 and Windows
2025 through `windows-host-contracts`. Cross-compilation does not establish its
runtime result. Actual full Gesher POSIX/NDK/ART networking, selected physical
interfaces, external resolver/filter behavior, long idle retirement and
representative throughput/latency remain distinct integration evidence. This
round does not claim completion of all NATIVE-10 or issue #178.
