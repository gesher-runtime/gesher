# RunTimed50 and InvokeOpenSl76 reader ownership: source only

2026-10-28, existing ADR0078/test84, owned C:/work/gesher-ci-reader-helpers /
wip/ci-reader-helpers from frozen82d7ce6e (current reviewed30db plus separately
qualified20/21 helpers). Active canonical30db/full40596167427 is unchanged.

Only actual50-services.ps1/RunTimed and76-media4-opensl.ps1/InvokeOpenSl gain
explicit token/read-task/owned-reader/pipe cleanup. Their original signatures,
120s defaults, each5s per-reader deadline, native statuses and failure markers
remain. CheckTimed/CheckOpenSl and every media/native assertion are unchanged.
50/StartTimed and50/WaitTimed are NOT modified; Surface file redirection, N2Run
and split helpers remain separate open resource scopes.

The exact previously proved native CloseOwnedReader C# primitive is relocated
unchanged into ProcessReaderHandles.ps1. GtestProcess imports that same code;
its ordinary process behavior stays unchanged and its43control baseline is
retained. New Close-ProcessReaders performs only cleanup after the callers'
original EOF verdict: cancel, finite5s task completion, capture only completed
late output, dispose both owned readers with paired immediate native invalid6.
Failed EOF remains failure. Canceled ReadToEndAsync prefixes remain unavailable.

Test84 upgrades the four real normal/exit23/timeout/retained cases for each
named helper with actual both-task/owned-pipe/process-handle observations and
independent invalid6 probes. A retained child must still be alive AFTER helper
cleanup, then exact-path owned cleanup is finite. No extra semantic case is
claimed merely because an existing case gains resource checks. Existing43
controls, including actual20/21 pipe controls, must pass unchanged.

Current checks are source/PowerShell/YAML parsing only; no local C#/native
compiler or executable fixture. Native peer and changed-source both-host CI
remain required. No full/libcore/ART/media semantic closure or arbitrary
kill-denial/job-containment proof follows from this source preparation.

## Qualified component, 2026-10-28

Push run 40655610161 attempt 1 tested source f5f8fe91af5376dc454db40e4600b291b837e6c3. Both actual Windows jobs passed: 122705781812 (2022, PowerShell 7.6.8) and 122705782131 (2025, PowerShell 7.6.7), both .NET 10.0.13. Each reports 43 existing PASS controls and zero test failures, plus eight actual reader cleanup observations: four RunTimed and four InvokeOpenSl (normal, exit 23, timeout, retained writer). Each observation requires both tasks stopped, both owned readers disposed with their exact captured pipe handles immediately invalid, process handle closed, and independent native invalid-handle probes. The retained writer remains alive after the helper while the original EOF deadline remains failure and status -1; owned child cleanup follows. Expected gtest failure lines exercise negative controls, not suite failures. Plain logs and metadata are saved under C:/work/gesher-ci-review/ci-reader-helpers-40655610161-*. No full integration, canceled-read partial-prefix, arbitrary kill-denial, or unrelated helper resource scope is established by this component.
