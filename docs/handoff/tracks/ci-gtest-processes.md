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
