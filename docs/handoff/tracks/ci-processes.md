# Bounded CI process and output cleanup

Coordinator-owned follow-up. ADR0078 and Windows test84 are reserved; no
CI acceptance exists yet. Work in the separate wip/ci-processes worktree.
Build and execute process fixtures only in CI.

## Status and next steps

Root's source review of the Surface invalidation candidate found that legacy
SurfaceRun/test76 process helpers can wait without a bound after attempting
to kill a timed-out process, and can wait indefinitely for redirected pipe
EOF. The new test80 helper has separate bounded process, post-kill and output
waits; that does not correct the older helpers. No observed hang is attributed
to this source finding without a reproducer.
The actual shared RunTimed in tests/windows/tracks/50-services.ps1 also has
an unbounded WaitForExit after kill and reads Task.Result after a timed wait
without checking completion. Its CheckTimed callers inherit that risk; the
same cases must be covered by the follow-up.

Inventory the real shared helpers before changing them. Preserve existing
test assertions and capture complete diagnostics when possible, but give
process exit, termination and output completion explicit deadlines. Exercise
normal exit, nonzero exit, timeout, retained descendant pipe handles and
failed/incomplete cleanup with actual CI-only subprocesses. Do not turn a
timeout or unfinished reader into success. Verify both Windows versions and
the unchanged full suite, package and e2e before claiming acceptance.

First source slice is on C:\work\gesher-ci-processes, wip/ci-processes from
verified13603d54. Only shared test50 RunTimed is corrected so far: preserve
native status/-1, bounded post-kill/output completion, no unfinished Result
read and finally disposal. Test84 compiles a real console fixture in CI and
checks normal/23/timeout/retained-descendant-pipes, including owned fixture
cleanup. The narrow ci-processes workflow invokes that actual helper without
running the rest of test50. No local compiler/native fixture was executed.
Scoped compilation/runtime, full CI and remaining Surface/test76 helpers
are pending. ADR0078 records the exact limited scope and remaining cases.
Scoped40524879609/c9a87f66 failed both hosts before compiling or executing
the fixture: csc CS1504 reports a missing source path ending in
tests/windows/Process84.cs. The corrected invocation resolves and logs the
actual absolute fixture/compiler/output paths before calling csc, avoiding
relative-segment interpretation. The helper's actual process behavior is
still unverified; the old failure is retained and not retried unchanged.
Changed-source40525679532/598cf01c compiles the actual fixture on both hosts
and prints all four normal/23/timeout/retained-pipe PASS records followed by0
test(s) failed. Its jobs remain red: the Actions PowerShell wrapper inherits
the intentional final negative helper result through LASTEXITCODE. The
scoped driver now explicitly exits its asserted failure count, exactly as
the full run-tests.ps1 driver already does. All case expectations and helper
status behavior remain unchanged. No full/product acceptance yet.

## Owner-requested stop checkpoint

Work stopped on2026-10-27. Changed-source scoped40526183946 at
ec0f003cc149a0f985b5f280924477dc20414872 is GREEN on both Windows hosts.
Root independently read actual122266298232/122266298625 logs: each compiles
the real fixture and has four mandatory normal/23/timeout/retained-pipe
PASS records, then0 test(s) failed and successful job conclusion. The last
driver exits its asserted failure count; all negative helper statuses remain
required. No full has been dispatched. Preserve the narrow source/proof;
SurfaceRun/test76 and other legacy-helper work remains open. Coordinate
broader full CI with the recurring Web3 investigation. No further changes
or dispatches after the owner's stop request.
