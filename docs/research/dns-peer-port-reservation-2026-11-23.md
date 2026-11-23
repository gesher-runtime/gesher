# DNS fixture: reserve UDP and TCP peer ports together

Date: 23 November 2026. Base: current main
`b2e1d97813e65e58d7e6dbd10ea21d1f821e405d`.

This repair changes the native DNS test peer's socket setup. The production
resolver, admission/worker limits, request ownership, wire tests and deadlines
remain byte-identical. It is a shared CI-fixture repair, not another efficiency
implementation or a completed #56 roadmap item.

## Observed failure and evidence boundary

The genuine manual fork run [45198148590, Windows-2025 job 137840987873](https://github.com/yom-chadash/gesher/actions/runs/45198148590/job/137840987873)
tested #361 head `4bb899914d9efb20b3b13067ad82acc323312722`. At
17:24:01 UTC it stopped at `FAIL DNS admission: peer TCP bind/listen`.
The runner had compiled production `network_dns.cc` with SHA-256
`99d5dc95ad3f08a952fa312e14db3b2b63eb6067c3caba57db48470a682d3b3f`.
That is also the production source used by the local and independent replays.

The failure occurs in the test peer's constructor, before its first production
DNS request. The old assertion combines `bind` and `listen` and records neither
the failing operation nor the native error. The historical Windows cause
therefore cannot be identified more precisely from that log. In particular,
the controlled collision below does not prove the exact old Winsock error.

The old fixture binds UDP to loopback port zero, reads the allocated port, then
binds TCP to the same number. A UDP reservation does not reserve a TCP address.
Microsoft's [bind contract](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-bind)
defines address ownership using protocol, address and port, and describes
independent UDP/TCP dynamic-port ranges. An existing TCP owner can therefore
make a legitimate UDP allocation unusable for this dual-protocol test peer.

## Repair and ownership

`dns_peer_ports.h` owns the prospective UDP and TCP sockets as one local lease.
It retains the UDP reservation while binding and listening on TCP, and publishes
the numeric port only after both are ready. The unchanged peer then takes each
socket exactly once and retains its original threads and teardown.

Only an actual native address-in-use result from TCP `bind` permits another
fresh reservation attempt. Each failed attempt closes its own sockets before
the next one. Sixteen attempts is a fixed ceiling. Allocation, UDP bind,
`getsockname`, TCP listen and other native errors fail immediately. No address
reuse option, sleep, global socket setting or resolver policy is introduced.

On failure, the fixture reports the exact operation, native errno/Winsock code
captured before cleanup, and attempt count. No successful result is substituted.
The sixteen-attempt ceiling bounds the number of reservation attempts; it does
not preempt an individual native syscall. The existing 75-second process bound
and all production/test deadline constants are unchanged.

## Actual-socket and unchanged-production checks

The maintained native DNS entry now begins with 101 assertions covering normal
joint ownership, a real competing TCP listener after UDP allocation, bounded
persistent collisions, propagation of a real invalid-socket error, failed and
successful socket retirement, and preservation of the competing owner's socket.
The control also permits a pre-existing address-in-use result when the OS has
already allocated that TCP number to an independent owner. Other setup errors
remain failures. Numeric native handles may be reused only for separately
tracked, still-owned collider sockets.

The ordinary entry then runs its complete unchanged production corpus. Both
the author and an independent reviewer passed 2,919 assertions under
AddressSanitizer and UndefinedBehaviorSanitizer. The 1,024-request burst still
admits 256, rejects 768, retains at most 32 blocked workers and 544 tracked
descriptors. Maximum-size TCP fallback and unconsumed-response ownership pass.
These are the existing declared Linux platform boundaries, not Windows runtime
execution. Local LeakSanitizer is disabled because this environment prevents
its startup; that setting changes no CI gate and proves nothing about leaks.

The complete fixture also compiles and links for Windows x64 and ARM64 with the
pinned LLVM MinGW toolchain and the existing warning-as-error settings.
The unchanged selected-policy, selected-supplier and vector-policy preflights
also pass, including the whole-file qualified manifests and shared-stdio reverse
normalization. No qualification hash or predicate changes. The newly inherited
main artifact-batch suite passes all 34 tests.

The archived negative control compiles the exact old peer setup from the
failed head and installs a real competing TCP listener after UDP's port choice.
The original combined assertion fails, and an added error-only observer records
Linux errno 98. This proves a sufficient socket-reservation failure without
changing the original predicate or any production resolver body.

Four separately compiled helper mutations must fail their specific controls:

| Mutation | Required rejection |
| --- | --- |
| Disable address-in-use recovery | A real first TCP collision cannot recover |
| Retry non-collision errors | An invalid socket is retried instead of propagated |
| Lose the UDP owner during cleanup | A failed attempt leaves an unowned live socket |
| Allow seventeen attempts | The independent sixteen-attempt control fails |

The ordinary full entry runs all peer and DNS controls. The explicit
`--peer-ports-only` option exists for focused diagnosis; no existing workflow
uses it. The runner now prints the fixture and both helper source hashes in
addition to the unchanged production source hash.

## Related repairs and release limits

The actual #394 diff concerns the Java `DnsResolver` callback/Executor retirement
fixture. It shares no changed path with this native peer repair. Its ownership
fix, assertions and deadlines remain independent prerequisites where needed.

The full current-head fork CI must still execute this change on Windows.
Cross-compilation, Linux replays and a reproduced sufficient collision do not
establish that the historical Windows failure had the same native error or that
every Windows test now passes. No efficiency percentage is claimed here.

The adjacent JSON manifest and compressed evidence retain the original CI log,
complete local/independent output, actual source inputs, native target build
receipts, original-source negative and all four compiled mutations.
