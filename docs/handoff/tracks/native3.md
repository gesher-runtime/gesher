# Track brief: native3

Follow-up to native2, verified in ci 40296960603 at d380a3e (both runners,
all jobs, package and e2e). Branch wip/native3, worktree
C:\work\gesher-native3, cut from integration after native2 merges.
ADR 0062 is reserved. Read track-rules.md and the current status sections
of native-libs.md, posix.md, arm64.md, plus ADRs 0011, 0014 and 0057 first.

## Scope

- Close public NDK API gaps reported by native2's `ndk:*` coverage lines,
  grouping the full missing-symbol lists by implementation owner. The
  real-app import gate passes; it covers a smaller set. Do not equate it
  with complete NDK API coverage or add dummy exports to satisfy counts.
- Platform libraries absent in the verified dlopen test: libjnigraphics.so,
  libmediandk.so, libcamera2ndk.so, libvulkan.so. Provide working API/ABI
  bridges and handles, by soname and system path. Existing AndroidBitmap
  implementations should be reachable through libjnigraphics as well.
- Own libc/libm/libdl/libz API and ABI gaps: the API lists include wide
  locale-suffixed functions, stdio extensions, binary128
  long-double functions and zlib stream/header/reset APIs. Include the old
  LLP64 value/layout audit (ADR 0011); passing symbol lookup is insufficient.
  Windows ICU's Unicode data version and property predicates can differ from
  the pinned bionic implementation (ICU4X 2.0.0-beta2 at android-16.0.0_r4):
  establish matching wide-character behavior, including combining marks,
  supplementary/new characters and supported Windows versions. AOSP's ICU4C
  is available but equivalence to bionic's ICU4X must be verified.
- Implement fcntl byte-range record locks (currently always granted),
  with conflict, blocking, unlock, close and cross-process tests; preserve
  flock's independent open-file-description behavior.
- Finish inotify: other processes' writes before close; IN_OPEN/IN_ACCESS/
  IN_CLOSE_NOWRITE and moved watched directories. Audit the remaining
  POSIX limits in posix.md: signal() dispositions across processes,
  /proc/<pid>, mapping names/inode/device metadata and permission behavior.
  Route locale/time-zone limits to their locale owner with an explicit brief.
- mremap currently moves anonymous regions by copying and rejects file views.
  Close the file-backed/shared-mapping limit without losing shared writes.
- ADR 0009 epoll/poll limits: edge-trigger behavior, nested epolls, readiness
  when polling an epoll fd, concurrent waiters and pipe polling. Coordinate
  socket/legacy resolver gaps with net4 (ADR 0070); binder readiness belongs
  to binder2. The 84 remaining libc network/resolver entries are assigned
  to net4 in the baseline inventory, not implemented concurrently here.
- ADR 0017: System V JVMTI environment/callback tables for ELF agents and
  linker namespace isolation. Verify actual agents and isolated dependency
  lookup; a JNI-only library test does not cover either contract.

Coordinate shared files through integration. Graphics/surface/HardwareBuffer
and font APIs overlap surfaces/graphics; media APIs overlap media3; network
resolver/network-binding APIs overlap web3/net; process APIs overlap exec3.
Assign each reported gap explicitly and keep this brief's ownership list
current. Full Vulkan support needs its own implementation/verification work;
an absent-library fallback is not its completion.

## Verification

Extend NDK-native tests for both x86_64 and arm64, run in dalvikvm and APKs
(single process and device), plus real library self-tests where relevant.
Verify error paths, data layouts, callbacks, ownership and cross-process
semantics. Keep native2, 21-posix, 40-native-libs, 42/43-exec and 47-arm64
green on both runners; package and e2e too. CI only for AOSP. Before final
CI merge current integration; tick only CI-verified roadmap items with run ids.

## Status and next steps

Current full CI: 40333703940 at
f24223c7508a56722bc4e06d29d79b34026f559f, including verified aot2 and
integration b9f93f0. Watching with interval 120; no result is claimed yet.
Superseded failing 40332803102 was cancelled after this run was dispatched.

Verified independent feasibility probe: ci 40338754636 at e36e922,
all three jobs pass, all eight exact binary128/ABI checks pass on each
Windows version with 0 test(s) failed. The coordinator personally read
both logs. This proves precision beyond binary80 and MS/System V crossings
with the pinned llvm-mingw, not the 135 remaining libm API implementations.
native3-probes.yml cross-compiles
an own-file binary128 arithmetic/MS-to-System-V ABI executable with the
pinned llvm-mingw and runs it on both Windows versions. It uses no AOSP
build and no local compilation. This does not implement or verify the
remaining long-double/complex NDK math APIs;
the current full font CI remains unchanged.

ci 40333703940: Windows2022 finished with two failures, both standalone
Native3FontTest runs. It initialized Typeface's hard-coded /system/etc path
instead of Gesher's actual SystemFontMap, so enumeration found the 59
shipped XML fonts/119 axes but no registered matching collection. The APK
checks pass in both modes/ABIs. The standalone test now uses the production
bootstrap and SystemFontMap.load with the same target SDK as native2app.
Font axis assertions also require meaningful wght/wdth/ital values and a
nonempty variable-axis set. No matching/lifetime/file assertion was weakened.
Windows2025's full result is still pending. Package cleanup and both required
TIC-80/Puzzles e2e results pass; both e2e summaries say 0 unexpected failures.
The next run also includes the six-file CI reliability patch from aot3
50f16a9: hash-verified atomic APK cache/official mirror fallback, four tiny
transport fixtures, and a completion/45-second process bound for the old
geocoder probe. Timeouts and all app/status assertions still fail normally.
These are additive changes outside native3's domain, awaiting full CI.

Diagnostic CI: 40328565752 at
9c4d3803722eba83475ebe2475b7457ded394f21, with surfaces/integration 8590f4b.
Native test compilation passed; the main build failed before Windows tests:
`hwui/MinikinSkia.h` was not found. Its pinned source is under
libs/hwui/hwui, so the private include root is corrected to libs/hwui.
CI now compiles this own bridge object before the full runtime so further
header/ABI errors are reported early. ci 40332803102 at e3e3a0b reached the
bridge early and failed on upstream Minikin is_pod deprecations and hwui
missing-override annotations. These two external include roots are now SYSTEM
includes; the bridge retains -Wall/-Wextra/-Werror. Current integration b9f93f0, including
verified aot2, is now merged locally; the combined tree needs a subsequent
full CI before merge. Do not treat this in-progress run as verification.

Next implementation, not CI-verified: the NDK's AFontMatcher/AFont APIs and
ASystemFontIterator use Typeface's real Minikin collections and font metadata.
The iterator prefers the registered font set and falls back to shipped
fonts.xml through TinyXML2; no Windows font substitutes are introduced.
Tests enumerate real readable SFNT/TTC files, collection indices, locales
and available axes, match mixed Latin/Hebrew UTF-16 runs, request italic
style and retain metadata after the iterator/matcher is destroyed, in both
ABIs and both APK modes plus framework dalvikvm. Current integration
8590f4b (surfaces included) is merged. No AOSP code was built locally;
the Minikin-linked bridge and native tests require CI compilation/runtime.

Verified: ci 40305973210 at e94f760a5a56593187fc8bd1bae77fc716f8ac97,
including integration 91c195d. Personally read every job (success/skipped),
both zero-failure Windows summaries, all bitmap dalvikvm and APK checks,
package install/run/uninstall cleanup and e2e (0 unexpected failures).
No retry. TIC-80 remains a known failure on this branch without surfaces.
The bitmap slice is verified; the rest of this track remains open and the
branch has not been merged. The scoped roadmap row cites this run.

Verified implementation: libjnigraphics' six bitmap entry
points over hwui's real ABitmap storage, with System V entries/JNIEnv
conversion and a compression callback trampoline. Builds AOSP's existing
apex/android_bitmap.cpp; patch 0620 handles the host's software-only Bitmap
getHardwareBuffer path. Tests extend native2app in both modes/ABIs and add
46-native3's framework dalvikvm Bitmap tests (direct pixel aliasing while
locked, PNG callback/decode round trip, data space and error paths).
Java test sources compile locally with JDK 21 against the saved framework
classes; PowerShell parses; the patch applies to the pinned source. No
AOSP build was run locally; C++/NDK linking and the listed runtime checks
are verified by that full CI run.

The baseline ownership inventory is now recorded in
`docs/handoff/native-api-coverage.json`, parsed from both runners in ci
40296960603 (jobs 121487355022 and 121487354891). Its 56 ABI/library/runner
records contain 1,157 distinct missing export names. Current owners are native3
(724), media3 (313), surfaces2 (25, ADR 0063), web3 (seven networking exports),
net4 (84 remaining libc resolver/network entries, ADR 0070)
and exec3 (four log/property/bootstrap exports). The four baseline bitmap
exports now cite ci 40305973210; the launcher-only __libc_init context cites
ci 40296960603 without claiming arbitrary-library dlsym availability.
All other entries remain unverified until they have relevant CI evidence.
Root coordinates property APIs with exec3 and socket/DNS files with web3;
do not implement those in parallel here. Continue the
common-shim and filesystem items above; do not equate the bitmap slice with
complete NDK coverage. No artifacts were downloaded.
