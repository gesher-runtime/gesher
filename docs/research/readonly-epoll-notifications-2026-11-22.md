# Timerfd and inotify producer notifications for EPOLLET

This implements the timerfd/inotify part of **N33 / T12 in #56**, reserved in
[the implementation audit](https://github.com/gesher-runtime/gesher/issues/56#issuecomment-7082880912).
The baseline is `972c2b42b40c4ac799e147f5e352d07ba615d0fb`, including merged
#249. Its native-fence registry changes and poll scratch remain intact. This
does not close the complete N33 row.

## Observable problem and measured work

Read-only event objects previously stayed on epoll's level path even when the
application requested `EPOLLET`. One unread timer expiration or inotify record
therefore made every subsequent wait return it again. Periodic callbacks also
kept rescheduling while the application had not consumed the first expiration.

The same fixture compiles the unchanged baseline and changed production. It
executes the entire `epoll.cc` and `timerfd.cc`, plus the exact inotify enqueue/read
bodies, over the declared host boundaries described below.

| Actual production work in the controlled trace | Baseline | Changed |
| --- | ---: | ---: |
| Repeated timerfd deliveries in 10,000 zero-time waits after the first edge | 10,000 | 0 |
| Repeated inotify deliveries in the same trace | 10,000 | 0 |
| Native callback invocations over 10,000 unread periodic deadlines | 10,000 | 1 |
| Native timer arm calls before the final read | 10,001 | 1 |
| Expiration ticks returned by that read | 10,000 | 10,000 |

Including the legitimate initial delivery, 10,001 returns become one. The
periodic trace eliminates 99.99% of callbacks without dropping ticks. These are
production work counts, not Windows wall-time, application CPU or throughput
estimates. New accepted inotify records still notify interested registrations,
including while the queue is nonempty.

## Android contract evidence

Primary references use Android common kernel tag `android16-6.12-2025-06_r38`.
They are contract references: the implementation is independently written, with
no kernel code copied or vendored.

| Reference | Relevant behavior | SHA256 |
| --- | --- | --- |
| [fs/timerfd.c](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/timerfd.c) | Expiration publishes input readiness and defers periodic rearming. Read forwards periods, returns ticks and restarts. Gettime forwards/restarts without draining; a later expiration can produce another edge. Poll returns IN. | `ceaef3134a95b9194e702c53bd4ab7ce4a6751eb16b492c9238c87b105e08e6e` |
| [fs/notify/notification.c](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/notify/notification.c) | Full/explicit overflow precedes ordinary merging. One overflow object stays owned anywhere in the queue until removed. Only newly inserted records wake waiters. | `3a756d5c49b822aa8d88c198866d6ab74aac7252c7f2f632cddbecf9d646f981` |
| [fs/notify/inotify/inotify_user.c](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/notify/inotify/inotify_user.c) | Inotify reports IN and RDNORM; reading removes complete records that fit the buffer. | `cac727f91df58b4b7611e0f2c4ad75c48a7ec71df5b1b6d684c704f8a4a89b96` |
| [fs/eventpoll.c](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/fs/eventpoll.c) | Initial registration/rearm samples current readiness; edge delivery consumes that registration's notification, and one-shot delivery disarms it. | `bef423c5c795a317e9492581212f161aa75d2080071fd4e9e05439819fb70dc8` |

The Linux oracle executes real timerfd, epoll, inotify and file operations on
Linux 6.18.57, including gettime without read. Its overflow case fills the real
16,384-record kernel queue using two persistent files and distinct removed-watch
records. A duplicate modification at capacity produces an overflow notification;
partial drain/refill leaves that marker in the middle, where another overflow
does not publish. Consuming it permits a later one. The oracle reads the actual
kernel capacity and bounds its fixture to 2–65,536 entries.

## Implementation and ownership

Read-only sources use the existing producer-generation subscriptions. Timerfd
selects canonical IN; inotify also selects RDNORM. Neither becomes writable.
Every registration owns its observed generation and initial sample. MOD replaces
that state; ONESHOT disables its subscription until rearmed. Ordinary eventfd
behavior stays unchanged. Consumed edges wait on the epoll interrupt channel,
without spinning on the source's still-signaled level handle.

Timerfd owns an `expired` state under its existing mutex. The first armed expiry
publishes and suppresses rearming; clock sampling still accounts for elapsed
intervals in constant time. Read drains and restarts. Gettime restarts an expired
periodic timer without draining, so a later expiry publishes even while old ticks
remain readable. Settime resets this state within the existing disable/join/
reprogram transaction. Clock selection, resume handling, errors, alias ownership
and native callback joins stay in their existing paths.

Inotify publishes after each accepted ordinary or loss record. One mutex-protected
overflow ownership bit is cleared only when the actual read removes that record.
Full-queue detection precedes merging, and an already owned overflow returns
before allocating an encoded record. This fixes the partial-drain/refill defect
found during independent review: checking only the tail could append another loss
marker and publish an extra edge. Record encoding, partial-buffer behavior and
level readiness remain intact; clearing marker ownership itself does not notify.

Lock order remains source then wake-channel for producers, and epoll state then
source for delivery. A producer never takes epoll state. Weak subscriptions do not
retain deleted registrations and use existing pruning. Delivery still validates
entry IDs after snapshots: DEL/close/numeric reuse cannot report the old entry.
No callback owns a numeric timer descriptor. Applications retain their real timers
and filesystem events; inputs, capabilities, permissions and success are not
substituted.

## Verification and real CI wiring

The final fixture passes **535 production-host assertions** and **137 actual
Linux assertions**, including 32 bounded snapshot/DEL/close/reuse races,
publication while already readable, masks, MOD, ONESHOT, independent epolls,
duplicate registrations, shared alias reads, non-consuming nested poll, no-spin
timeout, coalescing, overflow ownership and rearm. The same final fixture against
the unchanged baseline fails **85 assertions** and reproduces the table. Native
arm calls are also checked, so baseline/candidate total assertion counts differ.

The maintained `tools/test_host_event_loop_contracts.py --sanitize` now calls the
new gate alongside eventfd. Its actual final invocation passes 15 epoll fairness,
85 vector, 28 select and 88 scratch assertions; 153 production and 116 Linux
eventfd assertions; then the new 535/137. Existing timer validation passes 133
production assertions across ownership, errors, deadlines, callback quiescence,
suspend/resume and subscription failure, plus 68 Linux syscall assertions.
ASan/UBSan ran. LeakSanitizer was disabled only in the local environment because
executor ptrace prevents its final scan; the CI recipes are unchanged.

The real Windows `epoll_probe` includes the shared public corpus and supports
`--readonly-edge`. Its normal run executes it too, and the existing Windows runner
requires the new completion marker alongside prior markers. The public inotify
fixture observes each real publication through an independent epoll registration
before MOD/drain checks, accounting for asynchronous Windows completion without
draining or consuming the tested registration.

All changed production translation units and the real probe compile with warnings
as errors for **x86_64 and aarch64 Windows**, using the pinned unmodified publisher
LLVM-MinGW archive, Clang 23.1.3. Archive SHA256:
`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`.
This is target compilation, not a linked selected-supplier receipt or Windows
execution. Actual PowerShell 7.4.22 parses the changed runner. The unchanged
selected-policy preservation gate and 20 negative controls pass. No immutable
receipt, source pin, supplier, workflow authority or runtime gate was weakened.

```sh
python3 tools/test_epoll_readonly_edges.py --sanitize
python3 tools/test_epoll_readonly_edges.py --sanitize --source-ref 972c2b42b40c4ac799e147f5e352d07ba615d0fb
python3 tools/test_host_event_loop_contracts.py --sanitize
python3 tools/test_timerfd_contracts.py --sanitize
python3 tools/net-tcp-priority/test-selected-policy.py --selftest
```

The baseline invocation is expected to fail. Exact source hashes, commands and
captured work/contract output are in [the companion evidence](readonly-epoll-notifications-2026-11-22.json).

## Limits and remaining N33 work

The host fixture replaces Win32 events, CRT allocation and PollFds at the existing
declared boundary, controls native timer delivery/clocks, and supplies raw records
to the actual inotify enqueue/read bodies. It does not claim Windows scheduling,
directory completion, native cancellation, Android ART execution or suspend
hardware. Real Windows gates cover those paths; their execution is not claimed
by the local results.

Socket, pipe and nested-target edge contracts and EPOLLEXCLUSIVE remain open.
The existing general behavior that removes a watched numeric fd even if another
duplicate retains its open description is unchanged. Tests cover shared alias
state and explicitly remove an alias registration before closing it. This leaf
does not claim complete epoll or complete issue #56 compatibility.
