# 0065. aot3: compilation contexts, install profiles, secondary dex and oatdump

Status: proposed. No aot3 implementation has CI verification yet.

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

All are open until CI verifies them: oatdump compilation and behavior,
shared-library loader contexts, validated `.dm` install profiles and
secondary dex compilation/invalidation. No roadmap checkbox is closed.
No local AOSP build was run.
