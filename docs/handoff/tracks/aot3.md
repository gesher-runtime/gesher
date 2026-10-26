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

Verified oatdump stage: full ci 40368334642 at
96ef68790fc59098758ddead6b5f58be9eb5d648, attempt 1. The coordinator
personally read every job (22 success/six skipped), all four aot3 and all
14 aot2 PASS lines per runner, both zero-failure summaries, the actual
installed-copy/cleanup checks and both required Puzzles/TIC-80 e2e logs
(0 unexpected failures). The frozen stage report is aot3-oatdump.md.
Shared-library contexts, install-time .dm profiles and secondary dex remain
open. The later unverified cloud-profile follow-up stays on wip/aot3 and
is not part of this frozen stage.

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
