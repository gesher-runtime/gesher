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
