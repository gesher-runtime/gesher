# 0065. aot3: compilation contexts, install profiles, secondary dex and oatdump

Status: accepted for the oatdump/tool inspection stage, verified in full ci
40368334642 at 96ef687 (attempt 1). All four new checks and zero-failure
summaries pass on both runners; installed-copy cleanup and both e2e jobs
also pass. Shared-library contexts, cloud profiles and secondary dex remain
proposed/open. See [the frozen report](../handoff/tracks/aot3-oatdump.md).

Revision: install-time cloud profiles are also accepted, verified in full
ci 40408733247 at 05ca072. Both runners pass all 11 cloud checks and the
startup collector, with zero failures; actual package and both e2e results
pass. Shared-library contexts and secondary dex remain open. See the
[cloud-stage report](../handoff/tracks/aot3-cloud.md).

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

## Install-time cloud profiles

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
new checks are now verified in full ci 40408733247. The first local Java attempt could not
resolve the incomplete saved app/generated dependency set; the actual app
compilation is left to CI, with no local AOSP build.

First full cloud run 40375696804 proves validation/copy/profman acceptance
and selected compiled code on both runners, but is red: the fixture forced
interpret while requiring AOT execution, and malformed ZIP validation leaked
the input descriptor when StrictJarFile's native opening failed before its
cleanup try. The next source uses the production JIT/AOT path and framework
patch 0650 closes that failed-constructor descriptor. All remaining fallback/
cleanup scenarios still require the next full run; no cloud box is closed.

## Remaining contracts

Shared-library loader contexts and secondary dex compilation/invalidation
remain open. The scoped oatdump and cloud rows are closed with their
verifying runs; the aggregate remains open. The later boot-startup profile
supplement still requires full CI and the original performance criteria.
No local AOSP build was run.
