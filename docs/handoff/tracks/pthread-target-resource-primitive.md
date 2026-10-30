# Target resource retirement primitive experiment

The separate `probe-resource` supplier role enables `GESHER_RESOURCE_LEASE_PROBE`
with the routing macro. Existing roles keep the resource macro off and preserve
their private record layouts. The resource role has additional operation refs
and deferred clock retirement state; the supplier's actual layout receipt must
verify its size and fields independently. No public cancel or join implementation
uses the new lease yet, and this is not product activation.

The shared primitive acquires a valid target under the recursive registry mutex,
duplicates its CPU thread handle and start event, then increments the operation
reference before unlocking. A failed first duplication leaves no lease; a failed
second duplication closes the first owned handle. The controlled fault tests
pass NULL to the actual DuplicateHandle call, verify EAGAIN and exact handle
rollback, and leave target refs/resources intact. Those are injected failures.

All eleven clock-destruction sites use a helper that defers destruction while
operation refs exist. The wrapper preserves the owning clock pointer when the
destruction was deferred. Nine start-event close sites serialize close and NULL
publication with capture under the registry. The existing recursive registry
initializer permits the already-held retirement and release helper calls.
Neither TLS teardown nor ordinary retirement waits for an operation lease.

The fixture creates a native thread, attaches it to the provider and initializes
its clock through setcancelstate. A consumer captures a lease and waits outside
all provider locks. The controller then releases the native thread to ExitThread
and waits for actual kernel thread termination before resuming the consumer.
The consumer verifies owned handles remain usable, the old record is pending
retirement with no public ID, and the deferred owning clock remains lockable.
Last release must successfully destroy the clock and finish record retirement:
probe-only counters assert exactly one of each without reading the recycled
record. All fixture and duplicate handles close before the original +8 census.

This establishes one resource-only schedule when CI passes. It does not establish
all concurrent target APIs: several ended/p_state writers use different locks,
and setcancelstate/setcanceltype call test_cancel_locked while holding p_clock;
that helper currently performs a registry lookup. Public cancel also calls
pthread_self under the target clock, which can attach a fresh caller. Adding a
registry-to-clock nesting over these inverse paths is rejected. Public cancel
needs a separately reviewed state and cancellation/unwind protocol; self-cancel
must release any owned lease before its nonlocal exit. Join cancellation, all
target APIs, canonical suppliers and guest consumer binding remain open.
