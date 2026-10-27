# Gtest process cleanup: source only

2026-10-28, existing ADR0078/test84. Owned wip/ci-gtest-processes /
C:/work/gesher-ci-gtest-processes starts at reviewed canonical union
30db7709838839d28be68d8b467a57b26a3ac667. Its active full remains immutable.

Invoke-CoreLibsTest20 and Invoke-ArtGtest21 share only finite subprocess/output
completion. All real gtest parsing, positive passed-count requirement, native
exit-zero requirement, failed-test exclusions, failure diagnostics and original
900s/600s caller deadlines stay in their original tracks. The shared helper
adds finite5s process-tree kill completion and10s paired output completion,
never reads an unfinished Task.Result and disposes its actual process handle.
Incomplete output is failure even if the parent exited0 with a PASS marker.

Test84 adds a CI-compiled C# fixture for both actual helpers: real argv/filter/
working-directory checks, success, exit23 despite PASS, explicit failed case,
zero count, absent summary, actual timeout, missing executable and a real
exited parent whose independent child retains both pipes. Existing Win32
RetainOutput code is reused by that fixture; every owned retained child must
be observed alive, then verified by exact executable path and finitely killed.
The original Process84 behavior and all previously qualified controls remain.

Local checks are source/PowerShell syntax only. No C#/native fixture or compiler
is executed locally. Changed-source component on both Windows hosts and later
canonical changed-union full are required. Actual kill denial, job containment,
remaining legacy helpers and hardware-positive requirements remain open.

## First actual component and concrete resource follow-up

Frozen1a3b366fafae4a94593a7c2038f6190d03209924/component40597998338 attempt1
passed both actual jobs122518708798/122518709120. Each has43controls/0fail:
18new controls and all exact25prior40556470317 records. Exit23 despite PASS,
failed/zero/absent summaries, real timeout and exited-parent/retained pipes
were correctly rejected;16process-handle disposals perhost were observed.
Plain logs/metadata/control-review.json share C:/work/gesher-ci-review/
ci-gtest-processes-40597998338-. This is parser/bounds/process-handle proof.

A source audit found Process.Dispose intentionally leaves user-accessed
stdout/error readers owned by the caller; its closed process handle alone
does not prove pipe/task cleanup. The follow-up source passes explicit
cancellation through ReadToEndAsync, retains the original10s EOF failure,
then cancels and bounds actual read-task completion to5s, disposes both owned
readers and checks both pipe SafeFileHandles closed. Test84 additionally
requires actual GetHandleInformation/ERROR_INVALID_HANDLE on BOTH captured
native pipe handles, after both tasks finish, including while the retained
child is still alive. It logs actual PowerShell/.NET versions on each host.
Neither cancellation nor later cleanup can turn a failed EOF deadline into
success. This changed source needs peer and fresh both-Windows CI.

Primary implementation references: dotnet/runtime v9.0.0 Process.cs831–851
(caller-owned readers), Process.Windows.cs584–592 (sync redirected FileStreams),
RandomAccess.Windows.cs225–241 and Common/src/System/Threading/
AsyncOverSyncWithIoCancellation.cs (token-driven synchronous-I/O cancellation).
The earlier1a proof remains frozen with its explicit scope limit.
