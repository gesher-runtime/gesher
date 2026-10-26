# Verified stage: aot3 oatdump and app-image inspection

The remaining compilation contracts continue on wip/aot3 (ADR 0065). This
frozen branch contains only the fully verified oatdump/tool/fixture slice;
the later install-time .dm implementation is not included.

## Verification

Full ci 40368334642, attempt 1, source
96ef68790fc59098758ddead6b5f58be9eb5d648, already containing integration
d68ec79/f677944. All 28 jobs were personally reviewed: 22 success, six
skipped. Both Windows suites end with 0 test(s) failed. No rerun.

Actual new PASS lines on each runner (jobs 121737013561 and 121737013740):
- oatdump reads the shipped framework oat header and speed-profile filter.
- oatdump independently lists lifecycle.apk classes and compiled oat code.
- oatdump loads the app image with its boot extensions and finds LifecycleActivity.
- oatdump rejects a missing oat file without a crash.

Both also pass all 14 aot2 checks, including each APK-only/app-image startup
sample's actual ART load path. The fixture no longer moves compiled oat
directories, which could leave nested/missing artifacts after app startup.
Code inspection requires a nonempty compiled-code range and the actual
pinned SomeCompiled/AllCompiled class-state text.

Package job 121737013431 verifies the installed oatdump against the installed
speed-profile framework oat, then actual installed app/window/input and
device/data/association/toast/Start-menu uninstall cleanup. Both e2e logs
(121738478354, 121738478403) pass all nine apps, required Puzzles/TIC-80,
association/shortcut launch and all four pinned-byte transport fixtures;
both end with 0 unexpected failures.

The frozen head may add current integration docs and this scoped report
only. Check the code diff against the source SHA above before merging.

## Implementation and shared changes

Build the pinned ART oatdump and x86/x86-64 disassembler with the same
runtime/compiler libraries, package oatdump.exe and inspect actual app code
and images in 62-aot3. Additive shared edits: ci.yml early oatdump compile
(retaining the font bridge compile), package copy/installer entry and the
installed-copy inspection. The startup fixture change is in 61-aot2.

The verified tree also includes the coordinator's hash-checked APK cache/
mirror transport fixtures, completed-only Win32 geocoder probe termination,
exec3's complete atomic UTF-8 Launcher log lines with an executed host
regression, and media3's finite AAudio output prefill fixture. The geocoder's
genuine InvalidCredentials answer is read on both runners; timeouts still
fail. Tone/frame/frequency/duration and UI assertions remain unchanged.

No local AOSP build, WSL or local CI artifact download occurred. Review
logs are under C:\work\gesher-ci-review\aot3-40368334642-*.log.

## Remaining contracts

Shared-library class-loader contexts, install-time .dm profiles and
secondary dex compilation/invalidation remain open on aot3. The cloud-profile
follow-up at 13b6b42 is unverified and is deliberately outside this frozen
stage. This merge does not close the aggregate aot3 roadmap item.
