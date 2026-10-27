# Track brief: aot3

Compilation contracts left open by aot2 / ADR 0054. ADR 0065 is reserved.
Create branch `wip/aot3` and worktree `C:\work\gesher-aot3` from the
integration tip after aot2 merges. Follow track-rules.md; CI first, no local
AOSP builds or WSL, absolute paths for every edit.

Read docs/compiled-code.md, ADRs 0031, 0033, 0036 and 0054, the aot2 test and
boot-image workflow, com.gesher.server.Dexopt and ART's pinned class-loader
context/profile handling before implementation.

Goals, each verified on windows-2022 and windows-2025:
- Replace the hard-coded `PCL[]` context with the installed app's actual
  shared-library/class-loader context. Compile and load an app with a real
  shared Java library; assert ART accepts the oat/image and executes compiled
  methods. Cover library updates and invalidation without accepting stale code.
- Consume install-time `.dm` cloud profiles with Android's validation and
  fallback behavior. Prove the supplied profile selects real compiled app
  methods; reject mismatched/invalid metadata without corrupting installation.
- Compile secondary dex files from the owning app's data directory using
  their observed loader contexts, profiles and safe ownership rules. Exercise
  DexClassLoader execution, a later dex update and stale-artifact rejection.
- Build and package oatdump for the pinned ART and use it to independently
  inspect app images, oat code and compiler filters in CI, supplementing the
  current ART execution logs.

Domain: app dexopt/install/runtime glue, ART patches, dex2oat/profman/oatdump
CMake, package artifacts and focused tests. Add an own test file using the
next available test number after checking concurrent tracks. Keep shared
workflow/installer edits additive and list them in the final report.

Before final CI, merge current origin/phase4/integration. Read every job,
both zero-failure test summaries, the actual PASS lines claiming new work,
package install/run/uninstall cleanup and both required e2e results. Never
tick a box for unverified implementation. Remaining contracts become another
explicit brief rather than disappearing when this stage is green.

## Status and next steps

Corrected fixture scope40499783709 at immutablea79ae0f1 is complete green:
both jobs succeed; actual122174345315 read javac0/0errors, graphics-tests.dex,
sharedlibraryapp/sharedlibrarybad APKs and all three real DEX jars (549/546/687
bytes). Native peer independently matched all11 pinned-source audit digests
and found no blocking issue in graph/registry/SDK/Installer/Dexopt/stale-control
source. That is bounded static review, not runtime proof. Full40503913306
uses exactc35abe7c, with the same non-documentation tree as revieweda79 and
accepted runtime6297. The optional7b49 documentation merge did not occur:
that abbreviated object name was ambiguous; its earlier inclusion sentence
was incorrect. No code change, cancellation or duplicate dispatch follows.
The one complete full candidate now tests actual PCL/DAG loading, compiled hot
methods/app images, no-reinstall stale rejection, unchanged-APK refresh and
old-install preservation, alongside all original Windows/package/e2e gates.
After-resource/DLC actual app+image, secondary/static/SDK and arbitrary
factory-loader contracts remain explicit open scope.

Runtime graph/dexopt sourcee2e36137 compiles in scoped40470396915: both jobs
green; actual122075942761 has javac0/0errors, graphics dex and appjar570004.
Fixture compile40472620654 at7fe21800 is red in graphics-tests javac, after
production compilation: importing Main.bootstrap recursively pulls generated
service/compat sources outside that fixture's generated-server classpath.
The correction invokes the exact shipped Main.bootstrap reflectively at
runtime, retaining real ART/resources and every graph assertion. No production
or oracle change is made. Corrected compile/runtime proof remains required;
the old red run supplies no fixture or shared-library runtime acceptance.
Corrected continuation includes accepted Windows4/Web3 integration6297e80a
(merge1e3f05d3), preserving both additive framework build/copy groups. The
next scoped build verifies real dependency jars, both APK fixtures and the
ART graph controls before a full runtime candidate. AOT remains separate
from the frozen Net4/Windows sources.

The shared-library continuation is assigned to the Windows4/Web3/release/Net4
agent in this worktree. Root's cleanfb9aefb1 merge of integration808ea746 is
preserved. Test73 and ADR0065 remain reserved; secondary dex and APK-declared
static/SDK libraries remain separate open contracts.

Initial production source adds SharedLibraryGraph: immutable content-hashed
Java library nodes, one dependency graph for real SharedLibraryInfo objects
and ART PCL/DLC contexts, the same system-resource before/after partition as
LoadedApk, content rechecks and an oat-stage identity manifest. The actual
pinned10-argument SharedLibraryInfo constructor is used; library jars are
never appended to the boot class path. Ordinary empty PCL graphs avoid new
hash/provider work on the startup path.

The next source adds a permissions/sysconfig XML registry and wires graph
resolution into the newly parsed ApkPackage, so both single-process and
device ApplicationInfo receive the same actual SharedLibraryInfo graph.
Required/optional names, missing dependencies and cycles are handled before
an installer can replace its old APK. SDK gates use the real pinned
UnboundedSdkLevel, already bundled by framework-minus-apex under its actual
jarjar name android.internal.modules.utils.build; no SDK helper is copied.
Bootclasspath declarations must match a real current bootclasspath file.
Compile-only framework40468151378 atc51ed7d2 is complete green: actual
framework122068536860 read, javac0/0errors, gesher-app.jar569116 bytes and
graphics/app outputs built. This validates source compilation only.

The next source passes the new parsed package's graph directly to install
dex2oat, resolves the installed APK independently for background compilation,
and writes the content/context identity into the same atomic oat stage.
Jar contents are rechecked before compilation and before publication.
Unchanged-APK installs detect graph/content changes, stop the old app and
recompile; long-lived PM records resolve fresh runtime graph objects instead
of returning the previous jar/XML snapshot. Real jar/app/oatdump test73 and
library-update/stale-code/rejection evidence still must follow before full
acceptance. No compilation or fixture execution occurred locally, and no
AOT3 shared-library runtime behavior is claimed verified. Root priority remains the
active Net4 full and fresh Windows4/Web3 combinations after exit-freshness
acceptance; source is kept separate from those frozen candidates.

Prepared test fixtures now build two different actual dependency DEX jars,
an entry DEX jar and a real APK with direct calls through both separate
library loaders. A duplicate class in the entry jar/APK detects the real
before-app lookup order; a changed-required-library APK tests old-install
preservation. ART-side XML/SDK/cycle/missing-dependency and content-update
controls use those real jars, including a Unicode/space path. Framework
build/copy additions are separate additive lines. Prepared test73 requires
real oatdump hot-method code and app images, compiled execution with separate
loaders in both modes, both-mode stale-artifact rejection before reinstall,
same-path dependency recompile, and old-install preservation. To keep that
counterfactual stable, it temporarily defers the existing background optimizer
via its device policy, checks the actual server applies it, and restores the
owned build.prop bytes in nested finally cleanup. Earlier normal idle and
startup gates remain unchanged. Compilation/execution of these new fixtures
is still pending. The
pinned default config_sharedLibrariesLoadedAfterApp array is empty; serializer
support alone is not claimed as runtime after-app policy coverage.

Startup-profile stage fully verified: ci 40414859801 at
ca46e9967f8994fe329f3f10c88de7141e494f76. Root read every job, all 18
scoped PASS lines per runner, both zero-failure summaries, actual installed
input/uninstall and both required e2e logs (zero unexpected). See
[the frozen startup report](aot3-startup.md). Original size/timing gates
pass unchanged; this is not a before/after variance claim. The source
predates exec3/94ccde1, so their merge requires combined integration CI.
Shared-library contexts and secondary dex remain open.

Frozen cloud stage is fully verified in ci 40408733247 at
05ca072cad7a23d83403a0c039d47d4fec0c07b5. Every job, all 16 scoped aot3
PASS lines per runner, both zero-failure summaries, actual installed
input/uninstall cleanup and both required e2e logs personally read. See
[the cloud-stage report](aot3-cloud.md). wip/aot3-cloud adds docs only;
the later continuation on wip/aot3 is excluded from this stage. Shared-
library contexts, secondary dex and boot-performance diagnosis stay open.

Fast startup evidence is green in aot3-probes 40410450314 at bcddfb9:
both actual logs read, saved profiles 18673/18661 bytes, each dump 23314
rows/6666 methods including 3077 android-framework methods, zero failures.
Runtime artifacts are exactly full ci 40403284714/e928eec, validated before
use; this is a collector result, not a full-green or performance claim.

Prepared boot input tools/boot-image/gesher-startup-profile.txt adds 1971
observed common boot methods absent from the exact pinned AOSP profiles,
retaining observed H/S/P flags, and 177 observed declaring classes not
already listed. Excludes Gesher/app classes because gesher-app.jar already
uses speed; omits inline caches. Both runners' normalized method sets are
identical. SHA-256 053e8d37c550f79720919fe92cfbc559c9c28a2a7f7ccfeee3641a7098bc3c59.
New track 65 inspects PackageParser2.parsePackage and SystemFonts fallback
compiled code in the shipped framework oat. Original size and 10%/30ms
startup criteria are unchanged. This input is not yet compiled/CI verified.

The cloud stage is merged as integration 6dc3f03 (frozen head b085b7d,
docs only above verified source). This continuation includes current
integration a0f32e1. The prepared startup supplement is not yet verified.

Initial aot3-probes 40409910036 at 7bc3fd2 validates/downloads the selected
artifacts but both collectors fail before startup: the standalone root
omitted build.prop. Add the suite's framework-res/build.prop/font setup;
this failure says nothing about profile saving or boot performance.

A separate aot3-probes workflow is prepared to run the same real collector
quickly on both Windows versions using explicitly selected completed full
CI build artifacts (exact source SHA and all supplying build jobs checked).
It reuses the suite's actual helper definitions and track 64, without a
local runtime/build or unrelated test bodies. Probe results are startup
evidence only, not a full-CI/merge or performance claim.
Initial workflow dispatch returned HTTP404 because the new workflow is not
on the default branch. Its explicit branch/path push trigger uses recorded
source run 40403284714/e928eec for development; no main change is needed.

### Earlier collector history

The pending statuses below are historical; the fully verified cloud and
collector scope above supersedes them. The later startup supplement still
requires its own complete CI result.

Corrected combined ci 40408733247 at 05ca072 is pending. Source includes
current integration f520fd1; the sole new collector change enables its
required log and records actual size/marker diagnostics. All native
compilation and fixture execution now goes to CI.

ci 40403284714 at e928eec: all 11 cloud checks and four oatdump checks
pass on both runners; both original boot performance gates pass (JIT
411/430 ms on 2022, 469/478 ms on 2025). All other jobs pass/skip, actual
installed oatdump/input/uninstall and both nine-app e2e logs read with zero
unexpected failures. Each Windows suite still has one collector failure:
ART boot startup profile was not saved (exit 0). The fixture demanded the
saved-at-exit log marker without setting GESHER_LOG_EXECUTION, which gates
that marker in ART patch 0540. Set the flag and print actual file size/marker
before the assertion; do not infer an empty profile from that combined
failure. No timing threshold or runtime change. Combined CI remains required.
Latest integration f520fd1 includes the binding CI-only local compiler rule.

Combined ci 40403284714 at e928eec is pending. It includes integration
117629e, the supported startup-collector arguments, portable bounded TLS
fixture shutdown and only media3's bounded/canonical-mirror preparation
block (no recordapp dependency or test-asset changes). No new verification
or timing-cause claim until the actual Windows results are read.

ci 40389331018 at 31adc99: all 11 cloud-profile checks pass on both Windows
versions, including first app-image/AOT execution, failed-archive descriptor
cleanup, corrupt/mismatched/missing-profile fallback, identical-APK metadata
replacement and uninstall. Framework patch 0650 compiles in CI. All other
jobs pass/skip, but each Windows suite has one failure in the new startup
profile collector: the runtime rejects save-resolved-classes-delayed-ms:0.
That spelling appears in parsed_options help but not the actual pinned
ProfileSaverOptions parser; the actual delay/hot-sampling options are
deprecated no-ops. Remove both unnecessary arguments, retaining supported
boot/AOT profiling and save settings. No performance criterion changes,
and no full green/cloud roadmap claim until the corrected combined run.

ci 40385898668 at 9831182 failed before framework compilation: patch 0650's
Java source was absent from aosp.lock's sparse fetch. Add exactly
core/java/android/util/jar/StrictJarFile.java to frameworks/base's existing
paths, retaining every other entry. The failed run does not verify either
the descriptor cleanup or boot-profile learning. No compile error is
inferred from the missing-file patch-application error.

ci 40375696804 at 9ba2df5: real app/installer compilation, package and both
e2e jobs pass. Both Windows suites have two cloud-fixture failures. Matching
DM validation/copy/profman acceptance, selected onCreate compiled code,
malformed-update preservation and the old installed app's execution pass.
The fixture forced interpret while demanding AOT execution; it now uses the
production JIT/AOT path. Rewriting the malformed input then exposed a real
failed-constructor descriptor leak: StrictJarFile calls nativeOpenJarFile
before its cleanup try. Framework patch 0650 closes that owned descriptor
and marks the failed instance closed when native opening throws. The patch
applies to the pinned source; no AOSP compilation ran locally.

Repeated BOOT profile-vs-speed JIT failures in media3/web3 are owned here.
The 10%/30ms criterion is unchanged; no known-flake waiver or causal claim
for the unrelated app-artifact fixture fix. New 64-aot3-boot-profile uses
pinned ART's boot/AOT profile-saver options and profman to log the actual
framework startup methods as evidence for a targeted profile improvement.
The current supplied boot profiles and performance gate are unchanged.

Verified oatdump stage: full ci 40368334642 at
96ef68790fc59098758ddead6b5f58be9eb5d648, attempt 1. Personally read every
job (22 success/six skipped), all four aot3 and 14 aot2 PASS lines per runner,
both zero-failure summaries, installed-copy/cleanup and both Puzzles/TIC-80
e2e logs (0 unexpected failures). Merged into integration as f5ce6f6 through
the frozen aot3-oatdump branch; its head d038b75 adds docs only. The merged
tree differs from the verified source only in docs, so no extra integration
CI is needed. This continuation now includes current integration 634a273.

Prepared follow-up, not part of that run: install-time paired .dm archive
validation through pinned DexMetadataHelper, copied base.dm, unchanged-APK
metadata comparison and profman's real checksum/type/method-key validation.
Only successful staged profiles commit atomically; invalid profile data
falls back to verify without failing installation, while invalid ZIP
metadata rejects an update before replacing the current package. New
63-aot3-cloud covers actual selected-code inspection and app-image execution,
corrupt/mismatched/missing-profile fallback, valid-metadata replacement with
identical APK bytes, preservation after malformed metadata and uninstall
cleanup including interrupted import files. PowerShell parses locally;
actual app compilation and runtime checks require the next CI. The local
own-Java compile attempt failed because saved dependencies lack generated
app classes/android.system/dalvik classes; it is not a passing compile.
No local AOSP build was attempted. Shared-library contexts and secondary
dex remain open and are not silently covered by this profile implementation.
### Earlier CI history

Pending-run statements below are historical and superseded by this result.

ci 40352810247 at f949321: all four aot3 checks pass on both Windows
versions, including actual nonempty compiled code and app-image loading.
The only failure on each runner is the completed WinRT geocoder probe's
CLR/COM teardown after printing InvalidCredentials. All other jobs succeed
or are skipped; this is still a red run, not mergeable.

The next combined tree includes current integration d68ec79/foundation
f677944, the completed-only Win32 probe termination, exec3's atomic UTF-8
Launcher log framing with an executed host regression, and media3's finite
AAudio prefill fixture. Both oatdump and font bridge early compile checks
are retained when resolving ci.yml. JDK21's own LauncherTailTest passes
locally and the changed PowerShell files parse. No AOSP build ran locally;
all runtime/C++ verification still requires full CI. The first three
dexopt contracts above remain open even after the oatdump slice passes.

### Earlier CI history

The pending instructions below are historical, superseded by the results above.

First implementation on 2026-10-26: worktree C:\work\gesher-aot3,
branch wip/aot3, current integration b9f93f0 included. Added real ART
oatdump/disassembler CMake targets, installer entry/installed-copy check and
62-aot3's independent oat/app-image checks. Running full CI 40334251458 at
47224d88838189c43724746d1075199bb2d77cd5; watching at interval 120.
No local AOSP build and no CI verification yet. All four contracts above
remain open. Next: compile and
execute this first slice in CI before implementing the remaining dexopt work.

ci 40334251458 built oatdump and package successfully. Windows2022 e2e
failed before any app was installed: all nine F-Droid downloads immediately
returned `No such host is known`, producing 10 unexpected failures.
This is not a green run. Added hash-checked atomic APK caching and a mirror
listed by F-Droid, with own-byte transport/cache/error tests; no pins or
app assertions change. Full verification of this out-of-domain CI fix is
pending, along with both full Windows oatdump test results.
Also fixed 58-devices' known unbounded geocoder probe: it ignored a false
30-second Wait and then blocked on Result. It now checks completion first
and bounds the whole child process to 45 seconds using DevRun. A timeout
still fails the probe; no synthetic status or weakened assertion is used.

Final ci 40334251458: both Windows jobs have two aot3 failures. Framework
header parsing and missing-file rejection pass; both app inspections fail
because the single-process lifecycle.odex no longer exists after 61's
startup comparison. That comparison moved oat directories away and back;
a recreated destination can turn restoration into directory nesting.
It now runs an identical APK copy at a separate APK-only location, leaves
the compiled files untouched and requires ART's actual image-load marker
to agree with every measured mode. The installed oatdump header check and
package cleanup pass. Windows2022 e2e has the DNS failures above; 2025 e2e
passes. This run remains red and the corrected combined tree needs full CI.

ci 40344382617 at a61d3ec: both Windows jobs pass app-image loading,
framework headers, missing-file rejection and the strengthened startup mode
checks. The only failure on each is the class-state assertion: actual pinned
oatdump prints `(SomeCompiled)`, not `OatClassSomeCompiled`. Its output already
lists LifecycleActivity and code sizes 75/219 bytes. Corrected that enum text
and strengthened the code check to require an actual nonempty compiled-code
range, rather than any `CODE:` substring. Full CI is still required.
