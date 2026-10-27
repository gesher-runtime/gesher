# Track brief: native3-time

Close the native3 elapsed-clock gap using ART's existing monotonic branches.
ADR 0075, ART patch 0573 and test68 are reserved. Root owns branch
wip/native3-time in C:\work\gesher-native3-time. The implementation includes
native3 candidate b3ecc85 and integration 9dd57f5.

## Status and next steps

Full ci 40433469320 at c168b23 is running. The source selects
CLOCK_MONOTONIC for NanoTime, MicroTime and MilliTime and excludes their
unused wall-clock helper. Its actual linked-artbase fixture adds seven
independent Win32 QPC/order/elapsed/concurrent checks, with a wall-clock
counterexample. Existing CPU checks and original full-suite timing gates
remain intact. Patch application and PowerShell parsing were checked
locally; every native compile and runtime check is left to CI.

Before acceptance read every job, both zero-failure summaries, scoped PASS
lines, actual package and both required e2e logs. Tick only exact verified
scope. Root's corrected ARM width/CPU candidate is a separate prerequisite;
non-current pthread clocks, native malloc accounting and the remaining
native3 gaps stay open. Integration rule g still gates every further merge.
