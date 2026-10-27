# 0075. Use monotonic time for ART elapsed clocks on Windows

Status: proposed; compilation and runtime acceptance pending CI.

## Context

The pinned ART time helpers select CLOCK_MONOTONIC on Linux but use
gettimeofday on other platforms. Gesher still follows that wall-clock
fallback for NanoTime, MicroTime and MilliTime. Elapsed durations and
deadlines must not follow UTC clock adjustments or mix with an epoch-based
clock. Current-thread/process CPU clocks are separate contracts in patch 0572.

## Decision

Patch 0573 selects ART's existing CLOCK_MONOTONIC branches on Gesher and
honors InitTimeSpec's clock argument for absolute deadlines. The
unused gettimeofday helper is excluded on Gesher to keep -Werror clean.
The pinned llvm-mingw winpthreads implementation derives CLOCK_MONOTONIC
from QueryPerformanceCounter, with its existing GetTickCount fallback.
No clock implementation, scheduler policy or timing assertion is changed.

The native clock fixture links actual artbase and compares each function
against an independent Win32 QPC bracket in its own units. It checks
ordered samples, elapsed sleep, concurrent callers, absolute monotonic/UTC
deadline brackets with offsets and relative duration. A wall-clock
counterexample confirms the bracket rejects the former epoch domain;
the test never changes the machine clock. Both Windows runners must also
pass the complete original suite, package and e2e gates before acceptance.

## Limits

This does not implement non-current pthread CPU clocks, native allocator
accounting or change GC policy. No camera-performance improvement or
general timing-variance claim follows from this source change.
