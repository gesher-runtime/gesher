# Track brief: native3-time

Close the native3 elapsed-clock gap using ART's existing monotonic branches.
ADR 0075, ART patch 0573 and test68 are reserved. Worktree
C:\work\gesher-native3-time, branch wip/native3-time. Root owns this track.

Base includes native3 candidate b3ecc85 and integration 9dd57f5; corrected
unsigned ARM width, CPU patch 0572 and the bounded AAC prepare observation
remain separate prerequisite evidence. No local C/C++/Java compilation or
native/JVM execution is allowed; use CI for every build/runtime check.

## Status and next steps

Corrected full ci 40434741055 at bdfb230 is accepted, attempt 1. Root read
all 28 conclusions (22 success/six skipped), both zero-failure summaries,
actual time/CPU/whole-range ABI PASS lines, package window/input/uninstall
and both required nine-app e2e logs (zero unexpected). All ten new elapsed/
deadline checks retain the five CPU checks and original full-suite gates.
See native3-time-verified.md for exact jobs. Independent second source
review found no remaining helper/domain/unit issue before that full run.

Full ci 40433469320 at c168b23 was cancelled after independent source review
found InitTimeSpec still called the excluded wall-clock helper. No compiler
or runtime result is accepted from that source. The corrected patch also
selects its existing clock_gettime branch, honoring the requested deadline
clock, and adds direct monotonic/UTC offset and relative-duration checks.
The corrected accepted run verifies all ten checks and original gates.

The verified patch selects CLOCK_MONOTONIC for NanoTime,
MicroTime and MilliTime and excludes their unused wall-clock helper. The
actual linked artbase fixture adds ten independent Win32 QPC/ordering/
elapsed/concurrent/deadline checks, including a wall-clock-domain counterexample.
The original CPU clock checks and all full-suite timing gates are retained.
Patch 0573 applies after exact 0572; actual compilation, runtime tests,
package and both e2e results pass in the full run.

Before acceptance merge current integration and read every job, both zero
test summaries, all scoped PASS lines, actual package and both required
e2e logs. Tick only this scoped monotonic contract with its verified run.
Non-current pthread clock identity/lifetime, native malloc accounting,
file-backed mremap and the other native3 contracts remain open.
