# Native3 record locks: initial source/design checkpoint

Dedicated worktree/branch `C:\work\gesher-native3-locks` / `wip/native3-locks`
starts at integrationc455f432. Root reserved ADR0080 and Windows test87 after
checking canonical files for collisions. The source audit and proposed
contract are in [ADR0080](../../decisions/0080-native3-advisory-record-locks.md).

Current stage is design/source audit only: no production change, fixture,
compiler, runtime or CI result. Known shared seams are common fcntl's32-bit
integer argument versus record pointers, bionic's fake success, common fd
close/dup2/F_DUPFD lifecycle, and stdio's CRT close paths. A correct advisory
registry needs process creation identity/crash cleanup and stable file/open
description identity, without mandatory file-data locks. Independent design
and source review precedes qualifying CI. Preserve existing integer fcntl,
flock, x86 and all established tests. Unsupported fork remains explicit.

Coherent ARM API3 prerequisite remains independently active at40549806536 /
c7a43653 in the varargs worktree. Reviewed environment85 source is saved at
`wip/arm64-env-review-checkpoint`70beaaa5, production54cccc22/provenancee543d8ea,
and has no CI dispatch until coherent runtime proof. Neither is accepted as
a full product. No broad full is dispatched until relevant prerequisites and
Web3 gate pass. Next lock action: peer review the actual coordination design,
then implement and verify its bounded native/common and NDK contracts in CI.

Prepared Linux reference source838e90a0 executes actual libc fcntl through
independent CI workers, including POSIX/OFD owner distinction, ranges/errors,
real blocking/EINTR/SA_RESTART, cross-file POSIX deadlock versus OFD waiting,
hardlink/dup/stdio/failed-freopen lifetime, crash cleanup and exec descriptor
transfer. Peer review found lifecycle/query-normalization issues; source now
cleans a failed worker construction, attempts every worker cleanup with
bounded reader EOF, preserves nonquery/error PID evidence and asserts
unlocked-query input fields unchanged. AST parse only ran locally; no syscall
fixture ran. Reference scope still explicitly excludes injected close-EINTR,
simultaneous mutation contention and PID-reuse/denied-query controls. This is
not production record-lock implementation or final acceptance.
