# Libcore file-copy natives

Coordinator-owned follow-up. ADR0077 and Windows test83 are reserved; no
CI acceptance exists yet. Worktree C:\work\gesher-libcore-io is on
wip/libcore-io from integration3577b330. All compilation and native/JVM
execution belongs in CI.

## Status and next steps

Full40503913306 at c35abe7cccb3d2ae5896ce16933d5b6573eb6936 exposed a real
missing native on both Windows runners:

> java.lang.UnsatisfiedLinkError: sun.nio.fs.UnixCopyFile.transfer(int, int, long)

The AOT3 shared-library fixture reached Files.copy at
Aot3SharedLibraryTest.java:62 and failed before its graph/runtime assertions.
Each runner reported two top-level failures. AOT3's corrected fixture uses
real file streams to isolate its own graph checks; this does not implement
or verify the missing runtime native.

Read the pinned android-16.0.0_r4 UnixCopyFile Java/native sources and Gesher's
existing libcore registration and descriptor mapping. Implement the actual
contract, including descriptor errors, cancellation/progress and partial I/O
where required by that source. Exercise public Files.copy on real files,
empty/large content, failure preservation and the actual JNI entry point on
both Windows runners. Preserve the independent AOT3 graph/image oracles.
Before acceptance, merge current integration and verify a full CI run,
including package and both e2e sets. Keep any remaining I/O native gaps
explicit; no roadmap box is ticked for this observation.

Source inspection found the actual upstream UnixCopyFile.c is already in
the configured libopenjdk source list; only its name-found entry is missing
from compat/libcore/builtins.cc. The candidate adds that exact JNICALL
declaration/table row and retains the original native body. Test83 exercises
15 real public/native groups, including UTF8 paths, buffer boundaries,
offsets, descriptor ownership, read/EOF/error/cancellation ordering and
clearing a real cancellation word. ADR0077 records pinned hashes and the
uninduced short-write/EINTR limits. Source review, compilation and CI remain
pending; no local compiler or JVM/native fixture has run.
Bounded read-only peer review confirms the exact signature/table row and
all15 fixture groups match the pinned native's ordering; no source blocker
was found. PowerShell AST/diff checks pass only. A full CI run is next; the
baseline already contains current verified integration3577b330. No roadmap
box is ticked before actual compile/runtime/package/e2e verification.

## Owner-requested stop checkpoint

Work stopped on2026-10-27; full40522376074 remains on exact source
2913d484b4f3adee64acd44c5dd7b7e4a042d6f7. At the last required-watch
snapshot15:32UTC, build122252635829 succeeded37m44, both Windows tests
122275166484/122275166538 were active, and package122275166541 plus
e2e122276311510/122276311525 had successful conclusions. Root has not
independently read those package/e2e actual logs; no acceptance is claimed.
Next session must read final metadata and every actual critical log,
especially all15 CopyFileTest groups and both zero-failure summaries.
Watch log: C:\work\gesher-ci-review\libcore-io-full-40522376074-watch.log.
No code change, new dispatch, merge or roadmap tick after the stop request.
