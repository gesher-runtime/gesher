# Bounded CI process and output cleanup

Coordinator-owned follow-up. ADR0078 and Windows test84 are reserved; no
implementation or CI acceptance exists yet. Use a separate wip/ci-processes
worktree before edits. Build and execute process fixtures only in CI.

## Status and next steps

Root's source review of the Surface invalidation candidate found that legacy
SurfaceRun/test76 process helpers can wait without a bound after attempting
to kill a timed-out process, and can wait indefinitely for redirected pipe
EOF. The new test80 helper has separate bounded process, post-kill and output
waits; that does not correct the older helpers. No observed hang is attributed
to this source finding without a reproducer.

Inventory the real shared helpers before changing them. Preserve existing
test assertions and capture complete diagnostics when possible, but give
process exit, termination and output completion explicit deadlines. Exercise
normal exit, nonzero exit, timeout, retained descendant pipe handles and
failed/incomplete cleanup with actual CI-only subprocesses. Do not turn a
timeout or unfinished reader into success. Verify both Windows versions and
the unchanged full suite, package and e2e before claiming acceptance.
