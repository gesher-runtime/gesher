# Bounded working storage for outer poll and epoll

Baseline: `f93a90fac55dbe1aff641e6e3f26879292907cb2`.

## Problem and change

`PollFdsImpl` rebuilt four vectors of socket/event references and descriptor
indices on each call, plus a vector of Win32 wait handles when needed.
`EpollWaitImpl` and `PollEpoll` also rebuilt descriptor/generation snapshots.
These functions serve the runtime's event loops and native I/O waits. The
Winsock-side scratch reuse added earlier did not remove these outer allocations.

One small working-storage bundle of each kind is now retained per polling
thread. Every invocation acquires a lease. A nested invocation uses independent
local storage while the outer lease is active. On every return, including native
errors, the lease clears all elements before making the cache available again.
Socket descriptions and EventFd owners therefore survive their admitted sample
but never survive a completed invocation in cached storage. Cleanup itself may
reenter poll without borrowing the array being cleared.

Calls with more than 256 poll descriptors use local storage. An epoll interest
list can grow while a wait is active, so its lease drops any array whose capacity
exceeds 256 on return. The same capacity limit applies to every retained array.
This exchanges bounded idle storage for fewer allocations: on Windows x64 and
ARM64, the maximum retained array payload is 16 KiB for outer poll and 4 KiB for
epoll, plus vector/TLS bookkeeping. Actual retention follows observed capacity;
it is not preallocated. Thread exit frees those arrays.

Readiness sampling, descriptor identity revalidation, event masks, wake-channel
registration, deadlines, round-robin delivery, one-shot arming and generation
checks are unchanged. The per-wait EpollWaiter event is still created, registered,
removed and closed in the existing order. No result cache or new capability is
introduced. The scope is consistent with the existing application-visible
[poll contract](https://man7.org/linux/man-pages/man2/poll.2.html) and
[epoll wait contract](https://man7.org/linux/man-pages/man2/epoll_wait.2.html);
this change does not claim to complete every Linux epoll feature.

## Measured allocation work

The same host fixture compiles the actual production functions from the exact
baseline or candidate. Linux eventfds/poll implement the declared Win32/Winsock
boundaries; operator-new counters measure the C++ working arrays. Each workload
warms once and then executes 10,000 calls with GCC 14.2, `-O1`.

| Workload | Baseline allocations | Updated allocations | Baseline allocated bytes | Updated allocated bytes |
| --- | ---: | ---: | ---: | ---: |
| `poll`: 64 mixed socket/EventFd records | 240,000 | 0 | 40,320,000 | 0 |
| `poll`: one ready EventFd | 20,000 | 0 | 240,000 | 0 |
| `epoll_wait`: 64 ready entries, one result per call | 280,000 | 0 | 50,800,000 | 0 |
| `poll`: epoll descriptor with 64 ready children | 280,000 | 140,000 | 50,800,000 | 30,480,000 |

The nested workload still allocates its private inner-poll vectors; the outer
epoll snapshot is reused. The implementation deliberately retains only one
bundle per type/thread instead of accumulating caches for each recursion depth.
The default fixture enforces the corresponding allocation budgets. Running the
same fixture against the baseline fails all four allocation gates; its behavior
checks pass in measurement mode.

These numbers describe total requested C++ allocation bytes across the stated
calls, not simultaneous RAM, process RSS, Windows kernel handle allocations,
application startup or network throughput. Linux uses an eight-byte `nfds_t`,
while the Windows compatibility header uses four bytes, so the host byte totals
must not be presented as Windows measurements. Elapsed samples are printed for
inspection but are not acceptance thresholds or application speedup claims.

## Validation

The existing `tools/test_host_event_loop_contracts.py --sanitize` invocation
also runs the new scratch case, so the current CI preflight covers it without
a new optional gate. The full suite passed under ASan/UBSan:

- Existing production epoll fairness: 15 checks.
- Existing production vector routing: 64 checks.
- Existing production select behavior: 28 checks.
- New outer-poll/epoll scratch behavior and allocation budgets: 66 checks.

The new fixture exercises mixed descriptors, ignored/invalid descriptors,
snapshot ownership after close, reuse of a descriptor number with a different
socket description, native and Win32 wait errors, reentry during sample and
owner destruction, five-level nested epoll, historical graph cycles, topology
changes, one-shot rearm, event-creation failure, large calls/capacity shedding,
concurrent waiters, and unchanged finite deadlines through resampling and
topology wakeups. Controlled clock steps test timeout arithmetic; event and
socket I/O otherwise use real Linux kernel handles. Registry and topology edits
remain explicit fixture adapters.

Reproduce the new candidate checks and baseline measurements:

```sh
python3 tools/test_host_event_loop_contracts.py --sanitize
python3 tools/test_host_event_loop_contracts.py --case scratch --measure \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
# Expected failure: the unchanged baseline exceeds all four new allocation budgets.
python3 tools/test_host_event_loop_contracts.py --case scratch \
  --source-ref f93a90fac55dbe1aff641e6e3f26879292907cb2
```

Both complete changed production translation units (`fd.cc` and `epoll.cc`)
cross-compiled for Windows x64 and ARM64 using the project's pinned
LLVM-MinGW 20261006, with `-std=c++20 -O2 -Wall -Wextra -Werror -fno-exceptions`
and the existing compatibility headers/Windows definitions. This checks real
Windows headers, templates and both target ABIs; it is not runtime execution.

The local sanitizer environment disables LeakSanitizer process inspection
(`ASAN_OPTIONS=detect_leaks=0`); ASan/UBSan remain fatal. The CI command does
not disable leak checking. Native Windows/ART execution, native WSAPoll
scheduling and application-wide CPU/RSS workloads remain validation gates
outside these host measurements. No existing assertion was removed or weakened.
