# Nested epoll and interest-list wakeups

The compatibility layer now permits an epoll to watch another epoll and makes
epoll descriptors readable to poll only when their interest list contains a
reportable event. Polling does not consume the inner event or disarm ONESHOT.

Graph updates are serialized with descriptor publication and a graph mutex.
Self aliases are rejected, opposing concurrent additions cannot create cycles,
and chains are limited to five epoll instances, including additions above or
below an existing chain. Recursive readiness samples have their own bounded
thread-local active stack because concurrent reversals can otherwise expose
different historical graph directions to successive samples.

Each epoll waiter registers a private interrupt channel on its epoll object
before taking its first interest snapshot, including waiters on an empty or
eventfd-only list. Topology updates/removals signal every channel while holding
the same object mutex. The waiter retains the object across descriptor close,
unregisters its channel under that mutex and closes its handle afterwards.
Generic poll wake channels also wake socket/mixed pollers.

Registering only the generic poll channel left a gap after the epoll snapshot:
an update could occur before a waiter registered, and another waiter could
consume the shared auto-reset interrupt. The deterministic Windows probe now
pauses a waiter in that gap, adds an already-ready descriptor, lets a second
waiter consume the old shared interrupt, then resumes the first. This test
fails boundedly against the old shared-interrupt implementation and passes
with private epoll channels. Separate controls close the epoll during a paused
wait and check that 128 zero-timeout returns do not leak process handles.

References: [Linux epoll](https://man7.org/linux/man-pages/man7/epoll.7.html),
[epoll_ctl](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html), and
[kernel graph locking/depth](https://github.com/torvalds/linux/blob/v6.12/fs/eventpoll.c).
The implementation is independent Windows code, not copied kernel code.

The shared `tests/native/epoll_nested_test.inc` runs in the existing Windows
epoll probe and both native-lib ABI tracks. It checks empty/readable readiness,
ONESHOT nonconsumption/rearm, cookies, aliases, exclusive rejection, cycle and
depth errors, opposing concurrent additions, concurrent graph reversals and
two waiters observing an addition to an empty interest list. The Windows
probe passes, retaining its 200 pipe round trips both alone and beside a
socket. Both-ABI and full-host verification is recorded in the
[native QA report](native-stability-qa-2026-11-05.md).

Edge triggering, last-open-alias lifetime semantics, and general cross-process
epoll inheritance are separate remaining contracts. This change does not
claim their completion.
