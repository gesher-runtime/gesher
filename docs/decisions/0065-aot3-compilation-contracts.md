# 0065. aot3: compilation contexts, install profiles, secondary dex and oatdump

Status: proposed. Oatdump's four focused checks pass on both runners in
ci 40352810247, but that full run is red on the old geocoder teardown.
The next combined verification is ci 40368334642 at 96ef687. No full green
aot3 result or cloud-profile verification is claimed yet.

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

## Install-time cloud profiles (pending CI)

The installer validates a paired .dm archive with the pinned framework's
DexMetadataHelper before changing the existing installed package, and copies
it to base.dm next to base.apk. Metadata equality is included in the
unchanged-APK fast path, so new metadata is consumed even with identical APK
bytes. The pinned helper validates the ZIP; despite its stale comment, it
does not validate manifest.json package/version fields at this baseline.

Dexopt invokes the pinned profman --copy-and-update-profile-key against
the installed APK. Only its all-dex checksum/type/method match result (0)
commits the staged reference profile atomically and selects speed-profile
with an app image. Missing, corrupt, empty or mismatched profile data selects
verify and preserves installation; malformed ZIP metadata rejects the install
before replacing the current package. Uninstall also removes an interrupted
cloud-profile temporary file.

63-aot3-cloud exercises this through the real installer/tools and app process,
including independently inspected selected code, app-image execution,
malformed update preservation, invalid/mismatched/missing-profile fallbacks,
metadata replacement for identical APK bytes and uninstall cleanup. These
new checks are not yet CI verified. The first local Java attempt could not
resolve the incomplete saved app/generated dependency set; the actual app
compilation is left to CI, with no local AOSP build.

## Remaining contracts

All are open until CI verifies them: oatdump compilation and behavior,
shared-library loader contexts, validated `.dm` install profiles and
secondary dex compilation/invalidation. No roadmap checkbox is closed.
No local AOSP build was run.
