# Ordinary eventfd edge notifications (N33 increment)

## Result and scope

Ordinary eventfd EPOLLET registrations now consume producer notifications once
per registration. Successful writes publish input notifications, including zero
writes and writes while already readable. Reads publish output notifications.
ADD/MOD consider current readiness. Eventfd epoll registrations in both LT and
ET modes expose canonical IN/OUT readiness, without the normal-band aliases.
The mask correction is local to epoll; general poll and other backends are
unchanged.

The [Linux epoll manual](https://man7.org/linux/man-pages/man7/epoll.7.html)
describes edge delivery and independent registrations; [epoll_ctl](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html)
defines ONESHOT rearming. Producer notifications are visible in the kernel's
[eventfd source](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux/+/refs/heads/linux-5.9.y/fs/eventfd.c).
This is independent Windows code, with no copied kernel implementation.

This does not complete N33. Read-only timerfd/inotify edges, socket/pipe edges,
edge registrations targeting another epoll, exclusive selection, cross-process
inheritance and final-open-alias removal remain open. Windows and Android
qualification are also required before the roadmap item can be closed.

## Design and ownership

Native Linux passed an initial 20 observations while unchanged production delivery
at `826ba8a0e87335607350a7ced029b97cc232e324` failed seven. The reference reports
new events after drain/refill between waits and after writes while already ready.
A last sampled level cannot preserve these events.

Each ordinary eventfd records separate input/output generations. Each entry
retains source identity, delivered generations, initial/MOD eligibility and an
owned wake subscription. Distinct registrations have separate histories; aliases
of an epoll share entries. Numeric close/reuse retains entry-generation checks.

EventFd holds weak subscriptions to separately locked epoll wake channels.
Publication and registration prune expired references. ONESHOT disables its
producer subscription until MOD replaces it. Waiter retirement and producer
signaling use the same channel mutex to protect handle lifetime. The lock order
is epoll state, eventfd state, channel; producers never take epoll state.

Subscriptions exist before the interest snapshot, preserving notifications in
the snapshot-to-PollFds registration gap. Consumed edge sources use zero raw poll
interest, so an undrained manual-reset handle cannot cause a hot loop. Pending
edges force nonblocking collection; otherwise waiting blocks. Delivery resamples
under the epoll lock before consuming generations. PollEpoll uses reportability
without consumption, preserving level outer-epoll readiness and one-shot behavior.

## Maintained verification

The shared public corpus runs as native Linux reference, Windows epoll probe and
both guest libc fixtures through existing includes. It covers partial semaphore
drain, refill, repeated/zero/failed writes, masks, rearm, aliases, independent
registrations, maxevents, nested-level readiness and close/reuse.

The new host runner compiles the complete actual epoll.cc. Win32 events/clocks,
CRT reservations, unrelated socket/timer/inotify calls and PollFds are declared
host boundaries. Producer, registry, ctl, delivery and nesting are production
source. Its additional cases cover snapshot gaps, concurrent waiters, deadlines,
CPU use, handle cleanup, subscription replacement/retirement, deletion/close while
waiting and epoll-close lifetime. Repetition loops count as aggregate controls.
Blocking-boundary call counts check consumed and disarmed ONESHOT waits.

```sh
python3 tools/test_host_event_loop_contracts.py --sanitize
python3 tools/test_timerfd_contracts.py --sanitize
python3 tools/test_epoll_eventfd_edges.py --source-ref 826ba8a0e87335607350a7ced029b97cc232e324
```

The first command includes the new companion in the existing required host gate.
The baseline command is an expected failing control. Timer extraction includes
the new EventFd support types while retaining old-source controls. Author
ASan/UBSan runs use `ASAN_OPTIONS=detect_leaks=0`: default LSan is unsupported in
the traced environment, and no LSan pass is claimed. Host adapters and Linux
references do not establish Windows or Android runtime execution.

## Author results at the candidate source

| Execution | Result |
| --- | --- |
| Native Linux eventfd corpus | 116 checks, zero failures |
| Complete production epoll.cc with declared host boundaries | 152 checks, zero failures |
| Same maintained host corpus with original main source | 152 checks, 64 expected failures |
| Complete ordinary Linux event-loop reference | 182 checks, zero failures |
| Existing production fairness/vector/select adapters | 15 / 64 / 28 checks, zero failures |
| Production timer regression cases | 133 assertions across seven cases, zero failures |
| Native Linux timer syscall reference | 68 assertions, zero failures |
| Windows x64/ARM64 production and probe translation units | Four strict compilations passed |

The counts are separate denominators, not a compatibility percentage. No
Windows/Android runtime or LeakSanitizer pass is claimed.

## Windows gate and remaining qualification

The main Windows probe requires both edge completion markers and overall success.
Track40 x86-64 and Track47 translated ARM64 include the shared public cases.
The focused gate runs the real built production probe, propagates failure,
requires its completion markers and bounds the process at 30 seconds:

```powershell
./tests/windows/EpollEventFdEdgeChecks.ps1 -RuntimeDir ./dist
```

The modified production TU and actual probe cross-compile with project C++20
flags, strict warnings and LLVM MinGW20261006 for x64 and ARM64. These objects
are not a full-link/runtime qualification. Windows2022/2025, both guest tracks
and a pinned API36 Android reference remain pending.

## Integration with open work

#68 `d9fd890417831dd66a1eeb62e5618fc67dc97516` changes only the pipe include and
Pollable's LookupPipe term in epoll.cc; preserve those disjoint hunks. #73
`6659ae3abbef69b44bf71d576a627d1cca4f8759` changes vector extraction in the host
runner; this increment's sys import and trailing companion call are disjoint.
No internal.h, fd.cc, OFD/pipe implementation, libc dispatcher, held ARM64 work,
CI policy or intentionally canceled run is changed.
