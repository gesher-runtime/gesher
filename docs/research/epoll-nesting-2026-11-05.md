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

Each epoll waiter now registers its own wake channel, including waiters on an
empty or eventfd-only interest list. A topology change makes all waiters
refresh their snapshots. Previously one waiter could consume the shared
auto-reset event and leave the others waiting on an obsolete list.

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
socket. Both-ABI and full-host verification is recorded in the final QA report.

Edge triggering, last-open-alias lifetime semantics, and general cross-process
epoll inheritance are separate remaining contracts. This change does not
claim their completion.
