# Isolated deferred cancellation capability

This diagnostic role is separate from the product. The original supplier and
fault patches remain byte-for-byte at 67045833; seven original source roles and
all eighteen previous jobs remain. Only `probe-deferred` applies the additional
`deferred-supplier.patch`, then enables ROUTING, RESOURCE_LEASE, CANCEL_STATE
and DEFERRED_ONLY. CANCEL_STATE without explicit DEFERRED_ONLY and retained
resources remains a compile-time error. Asynchronous type/creation requests
fail closed without publishing output only in this diagnostic capability.
The original asynchronous implementations in the other roles remain unchanged.
This is not a product ENOTSUP policy or a waiver of asynchronous requirements.

The source-only complete eight-role stack reconstruction is recorded in
`C:/work/gesher-ci-review/pthread-deferred-complete-role-stack-check.md`.
Every original reconstructed source byte matches the preceding seven roles.
Both changed translation units have pinned final LF byte hashes checked in CI.
The calculated new private layout is 544 bytes, align16: resource fields120/124,
state SRW128, lifecycle136 and join owner144. CI prints the entire actual layout
before strict size, every field/bitfield offset and role-presence assertions.
These are expectations until actual compilation confirms them.

## State and resource graph

The sole nesting order is registry -> inline state SRW. Every new finite state
or type/testcancel/shallcancel path gets its attached self before taking state,
captures its cancellation decision under state, then unlocks before invoking
cleanup or nonlocal exit. The obsolete p_clock -> registry decision helper is
excluded. State holders perform no registry acquisition, allocation, user
callback or application/thread wait. Nonblocking SetEvent/ResetEvent and atomic
operations are explicit native boundaries. Resource close/destroy helpers run
after state release, including callers already holding the recursive registry.

External deferred cancel disables cancellation on its own actor before resource
capture. Capture duplicates the target thread/event and retains its generation
under registry; it then unlocks. A diagnostic controller can hold the call here
with no provider lock while the target really completes native ExitThread61 and
TLS teardown. The resumed call checks the retained state and returns ESRCH;
all owned resources release before restoring the actor's old state. Restoration
can nonlocally exit. Self cancellation similarly releases state before delivery.

Foreign exit and native TLS publish ended/lifecycle under registry -> state,
after user destructors execute outside locks. The normal pthread wrapper clears
TLS before publishing ended and releasing its final record reference, preventing
a later TLS callback from using a retired record. Tryjoin requires actual kernel
thread completion, including when logical ended is already set. Detach and
join-claim mutations share the state protocol. Initial state is established
before a supported caller receives the created/attached identity.

Join registers a cleanup record owning target/actor leases and its claim before
restoring caller cancellation state. It waits for target completion and the
actor's cancellation event; cancellation is checked before consuming the target.
Setup and commit mask cancellation, not the infinite wait. Cleanup unclaims and
releases without consuming the target. Finite name/kill operations use retained
target generations and state snapshots; name publication/copy holds registry
outside state. Scheduler snapshots use the validated owner path for host and DLL
calls, avoiding the raw host lookup path. Legacy naming keeps the owned kernel
handle open and verifies its native ID; no fallback is removed.

## Actual fixture and wait observation

The new process tests state/type outputs, probe-only async rejection with unchanged
outputs, self deferred cancellation with exactly one cleanup and destructor,
the public cross-image capture/real-native-exit crossing, and join cancellation
followed by a successful join of the original unconsumed target. It loads the
exact producer DLL path and verifies physical unload after all calls finish.
The original final handle census bound remains +8.

For the join wait, the source hook only signals an event immediately before the
single WaitForMultipleObjects call. It does not wait or acquire a provider lock.
The original target remains blocked and cancellation is withheld. A separate
PowerShell controller samples the specific joiner's native ID through the OS
process thread snapshot and requires Wait/UserRequest. Only then does stdin
release the fixture controller to cancel that joiner. The raw OS sample and
source phase together establish this finite actual wait; a start marker alone
does not. No claim about every wait or scheduler interleaving follows.

## Remaining scope

The general asynchronous protocol is open. Acquiring state before SuspendThread
does not prove that the target owns no other registry/loader lock. Hijacking a
thread inside those regions can abandon their original ownership. A TLS-entry
marker does not cover the OS-loader-acquire to callback-entry window. Primary
contract analysis and protection of all three required async-safe APIs remain
necessary; this capability never enables async operation.

All broader public API, cross-CRT, supplier consumer, guest ABI, constructor,
reentrant/nonlocal callback, process shutdown and canonical adoption requirements
remain independently open. Passing this component would qualify only its actual
diagnostic source, roles and finite observations.
