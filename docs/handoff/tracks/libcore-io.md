# Libcore file-copy natives

Coordinator-owned follow-up. ADR0077 and Windows test83 are reserved; no
implementation or CI acceptance exists yet. Use a separate wip/libcore-io
worktree before changing production code. All compilation and native/JVM
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
