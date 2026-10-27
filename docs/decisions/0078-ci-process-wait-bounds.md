# 0078 - Bound shared CI process and redirected output waits

**Status:** proposed, 2026-10-27. No CI acceptance yet.

## Context

RunTimed in test50 waits indefinitely after attempting process-tree kill,
and reads redirected Task.Result even when its timed wait did not complete.
A normally exited parent can leave a descendant holding the output handles,
so parent exit alone does not prove output EOF. Other legacy Surface helpers
have similar waits and remain assigned to this track.

## Decision

Preserve the original shared helper's native exit status and -1 failure
contract. Give post-kill completion and each output task five-second bounds;
read Task.Result only after successful completion. Treat timeout, unfinished
output or reader fault as failure and retain the available diagnostics.
Dispose the process wrapper in finally. Never turn an incomplete read into
success or wait indefinitely while reporting a timeout.

Test84 compiles a small .NET Framework console fixture only on CI, then
executes the actual shared helper for normal exit, exit23, a long-running
process and an exited parent whose real descendant retains both output
handles. The latter must return failure within the deadline; the test owns
and explicitly ends that descendant by its recorded PID and exact image path.
No process or compiler runs locally.

## Evidence and limits

Source/PowerShell parsing only; both Windows scoped jobs and later full
integration/package/e2e proof are pending. No observed application teardown
failure is attributed to this source defect without reproduction. This
slice does not guarantee termination of descendants after their parent has
already exited; it reports incomplete output as failure within a bound.
SurfaceRun/test76 and other legacy helpers need the same audit/correction
before the aggregate process-cleanup track can close. Job-object containment
or injected process-kill denial is not claimed by these four cases.
