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

Full candidate ci 40430425743 at b3ecc85 is running. It combines the
independently verified startup input, corrected unsigned ARM width oracle
and actual per-ABI complete digest checks, CPU patch 0572 and bounded AAC
prepare observation. Locked reference/Windows-helper ci 40426388584 at
5229764 is fully green and root inspected both reference digests/33 rows
and both 17-case binary128 helper logs. This does not replace full runtime
acceptance or implement the remaining binary128 libm functions.

CI 40418638643 at b15ecea is complete and red: 2022 has zero failures;
2025 has exactly two media failures (AAC/M4A asset prepareAsync did not
finish within the unchanged 5s wait, then 27/28 recorded tones). Both actual
ART CPU-clock host checks pass, as do both boot performance gates. This is
scoped clock evidence, not full acceptance or corrected ARM-width proof.
Media3 read the AAC case: INVALID_OPERATION follows the timeout, and the
worker's decoder-open log arrives about 6.4s after the start. It is not
classified as a tone-only flake.

The next candidate includes the independently verified startup input from
ca46e99/ci 40414859801 and media3's opt-in bounded prepare-phase observation.
It changes no MediaPlayer state, HRESULT, waiting/assertion or tone criteria;
100ms slow events are capped at 16/process and use steady_clock. Root reviewed
the patch and applied it; its compilation and any new AAC timing evidence
must come from the next combined full run. CPU/ARM correctness remains pending.

Unsigned reference ci 40423333599 at a449d02 is green; all four jobs and
the complete reference rows/digest were inspected. It retains the signed
digest 566ac5b76f65a70b and produces separate unsigned 3650764a32c5bf03.
Prepared NDK tests select the frozen data from the actual target wchar
type, compute the complete width digest through both actual ABI paths,
and use the correct per-ABI wcswidth invalid-character result. Guest
override/code and these stronger checks are not yet full-CI verified.

Additional explicit Native3 clock/GC contracts from source audit: global
NanoTime/MicroTime/MilliTime still use the Windows wall-clock fallback;
monotonic selection needs separate implementation and native-bracket tests.
ART Heap::GetNativeBytes also uses the non-bionic/glibc constant malloc
fallback on Windows; real native allocator accounting remains open. Media3
coordinates a zero-CPU ratio guard in ART0593 with root's exact CPU patch
0572; neither is a demonstrated FPS fix until actual runtime evidence.

ARM width correction is now open: the native3-wide target macro audit found
unsigned wchar32 on Android arm64, while root's width oracle assumed signed
on both ABIs. Full ci 40404678100/581e88f verified the signed oracle and the
old shared test expectations; it does not prove the missing unsigned
high-bit cases. Width roadmap closure is withdrawn. Root prepares a separate
unsigned pinned oracle and guest wcwidth/wcswidth overrides; the current
combined 40418638643 is retained as diagnostic/clock evidence, not mergeable
proof of the corrected unsigned-width contract. No local compile or run.

Combined full ci 40418638643 at b15ecea is pending. It includes verified
cloud/exec integration 94ccde1 plus current rule/brief docs, the previously
verified width implementation and the new real ART CPU-clock host checks.
No CPU-clock or combined-runtime claim until that run is fully read.

Full width ci 40404678100 at 581e88f is fully green: every job personally
read (22 success/six skipped), both zero-failure summaries, whole-code-point
width/classification parity, both NDK ABIs and four APK cases per runner,
actual package input/uninstall and both required e2e logs (zero unexpected).
See [the scoped report](native3-width.md). Locked-reference ci 40404677936
at that source also passes every old/new digest, row and lock check.
Independent width and CPU-clock static reviews found no additional issue.

Current source includes integration 94ccde1 plus docs-only a06d930 and the
CPU-clock follow-up 0572/host executable. The new combined full run must
verify that code before integration; the frozen width proof does not cover
it. Wide/multibyte/locale/numeric/time ABI work is delegated to
[native3-wide](native3-wide.md), ADR0073/test66. Root keeps the clock,
binary128, remaining NDK/POSIX and cross-thread clock lifetime contracts.

### Earlier preparation history

Pending width statements below are historical and superseded by the scoped
full-width verification above. Other native3 contracts remain open.

The Unicode/Process stage below is merged as integration 117629e. This
continuation starts from 7826cbe (the following README-only milestone).
Next scope: replace approximate wcwidth/wcswidth with the pinned bionic
decision order and ICU data. The initial CI-only reference uses the locked
ICU4X wrapper, every code point/four invalid values and 33 targeted rows.
No production-width verification is claimed yet. Preserve all existing
Unicode classification/case digests and their separate provenance.

Initial width reference ci 40403945297 at f8871df is fully green: all four
jobs read, old 14 classification/case digests unchanged, 33 width rows and
whole-code-point digest 566ac5b76f65a70b recorded separately, both binary128
17-case/zero-failure logs read. Prepared production follows the pinned
wcwidth decision order with the pinned ICU4C data, including signed wchar_t,
Hangul/ignorable ordering, supplementary marks and special CJK widths. New
host parity and both-ABI native2/APK checks cover widths, bounded strings,
NUL termination and invalid inputs. Also import portable bounded TLS cleanup
and only the verified media-preparation block. Runtime verification pending.

Prepared CPU-clock follow-up, outside that frozen stage: ART patch 0572
uses the supported thread/process CPU clock paths in libartbase/time_utils
on Windows, replacing the global -1/thread and wall-time/process fallbacks.
Failures return an explicit sentinel without reading an uninitialized time.
The new own host executable compares actual ART values with independent
GetThreadTimes/GetProcessTimes brackets, including a busy worker and a
waiting caller; no AOSP build ran locally. The patch applies to the exact
pinned source, but compilation/runtime verification is still required.
This makes no claim about camera FPS or boot-timing causality.

Thread::Current()->GetCpuNanoTime() already uses the current CPU clock;
the separate pthread_getcpuclockid implementation still ignores a requested
non-current thread, so cross-thread clock selection remains open here.
Also retain ziparchive's observed Windows SetCloseOnExec-not-supported
diagnostic as a descriptor-inheritance contract to audit, not a package
smoke-test failure or an assumed out-of-memory error.

Verified Unicode/Process stage: full ci 40386670586 at
6f6ac6fa9519207797ee5b997e0ae1e5b9e77049, attempt 1, including oatdump
integration 634a273. Every job personally read (22 success/six skipped),
both zero-failure summaries, complete code-point/locale comparison, both NDK
ABIs and four APK cases per runner, existing Bitmap/font/Process checks,
actual installed input/uninstall cleanup and both required Puzzles/TIC-80
e2e logs (0 unexpected failures). No retry. See native3-unicode.md.
Current integration cd0b849 adds docs only and is merged. Subsequent tip
changes must remain docs-only before this merge. Remaining NDK/POSIX scope
above stays open on wip/native3; this does not close the aggregate. Continue
missing APIs/ABI/filesystem contracts after this verified stage is merged.

### Earlier CI history

Pending/unverified statements below are historical and superseded only for
the specific Unicode/Process scope verified above.

ci 40381742392 at 3c9f6cb: production Unicode bridge and host executable
compile/link, package and both e2e jobs pass. Both Windows suites pass the
new Unicode boundary/handle/error checks through both NDK ABIs and all four
APK cases, then abort before the full-code-point comparison: the test job's
sparse checkout omitted probes/native3/unicode/reference.json. Add that
directory to the checkout; retain the complete comparison and all suite
assertions. This is not a full green run or all-code-point parity proof.
The locked reference itself is fully reviewed at ci 40376822140/7ee6b32:
all four jobs succeed and all 15 lock/digest/boundary verification PASS
lines were personally read. This remains separate from Windows parity.

Prepared Unicode production follow-up, not yet CI verified: replace Windows'
changing ICU/16-bit CRT predicates with an isolated bridge over the pinned
ICU4C data, matching bionic's ASCII and binary-property/category rules. Plain
and locale forms share that bridge; add missing iswalnum_l/iswgraph_l and
wctrans_l/towctrans_l, including invalid-transform EINVAL. A native host
executable computes all-code-point hashes through actual System V shim
entries for all 14 plain/locale functions, compared with the independent
ICU4X oracle. Native2 adds 11 full-width boundary rows through dlsym,
wctype/wctrans, locale aliases and errors in both ABIs/process modes.
ICU4C equivalence is a hypothesis to be tested by these hashes, not a claim
based on the library name. No local AOSP/ICU build ran; PowerShell parses.
The branch now includes verified oatdump integration 634a273. The earlier
Process run below remains valuable but does not verify this new source.

Independent Unicode reference verified in ci 40374405950 at c5a9f95:
all four probe jobs succeed; the pinned bionic ICU4X wrapper and exact
2.0.0-beta2 crate/data versions produce 14 whole-code-point predicate/case
digests and 11 boundary rows. The coordinator personally read the reference
and both 17-case binary128/zero-failure logs. The exact Cargo lock is saved
from the reference log, checked against its SHA-256, with all eight ICU
dependencies pinned to beta2. Future fetch/run use --locked. This is not a
production change or proof of Windows ICU parity; that gap stays open.
No Rust/AOSP build ran locally and no Actions artifact was downloaded.

Verified Process follow-up: full ci 40367959106 at
644e7f8c6596b0de10233f947504f2bcf5a5de98, with integration f677944. Every
job personally read (22 success/six skipped), all five native3/four APK PASS
lines per runner, both 0 test(s) failed summaries, actual package input/
uninstall cleanup and both required Puzzles/TIC-80 e2e logs (0 unexpected
failures). No retry. New Unicode source and the newer oatdump integration
were not part of this run; the combined branch needs the next full CI.

ci 40365301342 at 9361064 was cancelled after media3's diagnostic run
40359757434 exposed four early native/Looper/resource failures: the new
readProcLines registration lacked its lightweight tests/dex-natives Process
declaration. That declaration is now included. No green result is claimed.

The same detailed daemon stack exposed missing Process.readProcFile in
post-GC memory metrics. The follow-up implements readProcFile and
parseProcLine using android-16.0.0_r4's common field/output-index contract,
with dynamic file reads capped at the pinned 64 MiB limit. Both natives
have matching lightweight stub declarations. Native3ProcessTest adds real
procfs memory pages, long files, signed/full-width integers, floats, quoted
and parenthesized strings, combined separators, character output, shared
array indices, truncation, sanitized bytes and argument errors. Its own
Java sources compile with JDK21; runtime/C++ verification requires CI.
No AOSP build ran locally. The camera's pacing deficit remains unproven;
no capture threshold was changed.

New independent fix, pending CI: media3 exposed an UnsatisfiedLinkError for
android.os.Process.readProcLines in ReferenceQueueDaemon. The explicit
Process table now registers a parser over gesher-base's real procfs, matching
the pinned Android numeric-prefix/array/error behavior. Native3ProcessTest
covers live Threads/VmRSS, 64-bit values, missing fields/files and invalid
arguments. Java compiles and PowerShell parses locally; no AOSP build was
run locally. Do not infer that this fixes camera pacing; media3 will retain
its counters and original capture thresholds.

Prepared shared CI fixes: the finite AAudio output fixture now preloads all
8820 frames before start, retaining exact frame/frequency/duration checks;
this is the source-only ac12cbb slice with scoped output proof on both
runners in ci 40330465222, not a green media3 claim. The completed WinRT
geocoder probe now flushes its genuine result and terminates only itself
through Win32, retaining 30/45-second failure bounds. Launcher child logs
are forwarded as atomic complete UTF-8 byte lines, with truncation/final
fragment handling and an executed JDK regression in exec.yml. These minimal
outside-domain changes need the next full combined run; UI assertions and
recording thresholds are unchanged.

Font/bitmap run 40344682373 at 1e881b5 is fully reviewed and green on attempt
2 after the permitted failed-job rerun of the known output tone timing
check. The frozen foundation merged into integration as f677944; the newer
rounding probe and process implementation do not inherit that full result.
The expanded binary128 rounding probe is separately verified in ci
40355164049 at d02ad7c: all 17 checks and zero-failure summaries personally
read on both Windows versions. This still does not implement libm APIs.
The frozen report is in [native3-foundation.md](native3-foundation.md).
The coordinator continues all remaining NDK/POSIX scope on wip/native3;
the merged foundation does not close those contracts.

### Earlier CI history

Pending-run instructions below are historical and superseded by the
authoritative results above.

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
