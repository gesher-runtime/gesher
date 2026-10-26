# 0065. aot3: compilation contexts, install profiles, secondary dex and oatdump

Status: accepted for the oatdump/tool inspection stage, verified in full ci
40368334642 at 96ef687 (attempt 1). All four new checks and zero-failure
summaries pass on both runners; installed-copy cleanup and both e2e jobs
also pass. Shared-library contexts, cloud profiles and secondary dex remain
proposed/open. See [the frozen report](../handoff/tracks/aot3-oatdump.md).

ADR 0054's app/profile/image model is verified in ci 40320549410 at ae75766.
Its remaining contracts belong to this track, as listed in
[the brief](../handoff/tracks/aot3.md).

## First implementation

Build pinned ART's oatdump.cc and x86/x86-64 disassembler against the same
art_runtime/art_compiler as dalvikvm and dex2oat. This adds no replacement
oat/image parser. Package oatdump.exe next to dex2oat/profman and exercise
the installed copy against the shipped framework image's oat file.

62-aot3 inspects the framework compiler filter, app classes/compiled oat
code and the real app image produced by 61-aot2, then rejects a missing file.
These checks supplement ART's execution logs; listing compiled code alone
does not prove that an app executed it.

## Remaining contracts

Shared-library loader contexts, validated `.dm` install profiles and
secondary dex compilation/invalidation remain open. Only the scoped
oatdump row is closed with its verifying run; the aggregate remains open.
No local AOSP build was run.
