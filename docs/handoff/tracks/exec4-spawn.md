# Exec4 ordered spawn child actions

Coordinator-owned worktree: C:\work\gesher-exec4-spawn, wip/exec4-spawn.
ADR0076 and Windows test77 are reserved. No local compiler or executable
fixture; every build/runtime check runs in CI.

Complete the pinned android-16.0.0_r4 spawn.cpp child action contract:
apply open/close/dup2/chdir/fchdir in list order inside the actual new process,
preserve the parent's descriptors/directory, ignore close failure, and report
action/exec failure through a real child's exit127 after spawn returns0/PID.
Preserve negative registration EBADF/errno gates from the separate test74.
Exercise actual self children for both NDK ABIs, not a reconstructed action
list or synthetic wait status. Check directory-relative opens, duplicate
ownership, failed-action short circuit and relevant close-on-exec handling.

## Status and next steps

2026-10-27: branch starts from action-registration source3e9f2e0d plus
docs a8028409, then imports integration30deb08c (fresh exit results).
Source audit confirms the old DoPosixSpawn opened files in the parent,
swallowed open/dup2 errors, replaced chdir paths without ordered resolution
and never executed fchdir. Pinned bionic performs these in its child and
exits127 on the first non-close failure; the parent returns0 and the real PID.
The first implementation uses a versioned actual-child launcher bootstrap,
bionic descriptor/directory APIs, restored close-on-exec metadata and the
caller's PATH independent of replacement envp. ELF/sh/tool programs enter
inside that actual process. Source a2576f49 received bounded read-only peer
review. It found absent stdio slots were transported as NUL, allowing a dup
from a closed stdin to succeed. The child now closes those transport-only
placeholders before restoring flags or applying actions. New actual closed0/
failed-dup0 controls and UTF-8 relative directories cover this finding.
Real NDK self-child fixtures now cover both ABIs: ordered relative
actions, child-reported PID, six first-failure/exit127 controls, ignored close,
close-on-exec same/copy cases, inherited directory fchdir, ABI O_DIRECTORY,
actions before failed exec, caller PATH versus replacement envp and parent
directory/flag preservation. Test77 uses the unchanged120-second process-tree
bound. exec.yml adds both ABI builds and a narrow branch/path push trigger;
the workflow is absent from main, so direct dispatch cannot bootstrap it.
Scoped40472829064 at5f2653c6 built both NDK callers. Corrected scoped
40497922800 at47733cd3 also succeeds; actual job122167992203 is awaiting
root log review. Full40498406783 runs at exact
e8e827e599077f994be650d1e7b7e40cc78b193c after importing accepted
Windows160e25e8 and integrationdb847. Watch log is
C:\work\gesher-ci-review\exec4-spawn-full-40498406783-watch.log, interval120.
Host bridge compilation and actual Windows runtime remain pending; nothing
was compiled/executed locally. No roadmap/runtime claim is closed.

Further source review found UCRT's documented unbound-stdio sentinel is-2,
which the old inherited-fd scan treated as a real HANDLE. The corrected
snapshot and child sweeps exclude exactly NULL/-1/-2. An actual Windows
no-console probe starts with STARTF_USESTDHANDLES/null stdin and no CRT
inherited-fd block, requires raw _get_osfhandle(0)==-2, calls the real shim
spawn/wait entries, and requires a real NDK child to see closed0 and exit42
while the parent's raw sentinel remains unchanged. Both ABI callers are
mandatory in test77; main CI builds this host target and launcher early.
This is a source finding, not an error observed in the earlier run.
Full40498406783 is superseded by the corrected source before acceptance.
Corrected full40502225202 is dispatched at exact
0808ba71b62ea784432de52f31bd3f86c9e2e3ad after merging accepted current
integration7b49a718 (docs above Windows6297/160). The second bounded peer
review of6eeb773d found no blocker; neither review supplies runtime proof.
Watch C:\work\gesher-ci-review\exec4-spawn-full-40502225202-watch.log,
interval120. Full40502225202 is red on both Windows runners: each ABI's
ordered Unicode directory actions and inherited-directory fchdir exit127
rather than42; the PID-file assertion consequently fails too. Both no-console
probes observe real raw stdin handles88/104/108 rather than the expected-2.
Each whole suite has4 top-level failures. Expected child127, first-failed
action short circuit, close-on-exec, caller-PATH/replacement-env and closed
parent stdin controls pass. No cause or child-action acceptance is claimed.

The scoped exec4-startup-probes workflow records actual CRT/Win32 startup
handles for NULL/INVALID stdin under CREATE_NO_WINDOW/DETACHED_PROCESS and
the exact toolchain's chdir assembly relocation with the wrapper's header
order. It changes no production code or test expectations. These diagnostics
must identify an actual unbound startup state before correcting the fixture.
Scoped40516917875/b2cb26cd compiles and its exact assembly jumps to chdir,
not an imported __imp_chdir; the ANSI-import hypothesis is unsupported.
The first standalone Windows2025 step exits1 without output and is not
startup evidence. Corrected probe packaging links libc++ statically, records
exit codes, compares console/GUI subsystem startup and disassembles the
actual failed CI launcher inside CI. No production fix is inferred yet.
Scoped40517826361/ef5a90ba compiled the static console/GUI probes, then failed
at inspection because released executables are stripped and llvm-nm reports
no symbols. No Windows startup step ran. The corrected inspection uses the
failed run's real dalvikvm.map and corresponding binary for the same compiled
bionic wrapper; that is not a Gesher-exec link-map proof. New opt-in launcher
diagnostics distinguish symbol lookup, flag restore/sweep, individual actions
and later program lookup, preserving child127 and all original assertions.
The exec4-child-diagnostics workflow builds only the real launcher/host
caller and runs unchanged test77 on both Windows hosts, reusing immutable
NDK/ARM artifacts from40502225202 inside CI. It executes no AOSP/JVM build,
package or e2e acceptance, and cannot replace final coherent full CI.
Scoped40519233739/9a90c55d succeeds in all three jobs. Root read actual
build122241434258 and Windows122241590060/122241590177 logs: console
CREATE_NO_WINDOW with NULL input acquires a real console handle92/104,
whereas DETACHED_PROCESS with the same NULL input naturally has raw-2,
Win32 NULL input, reserved0 and fileno-2 on both hosts. The no-console
fixture now uses DETACHED_PROCESS and retains exact raw-2, actual shim
spawn/wait, child closed0/exit42 and unchanged parent assertions. This fixes
the fixture's startup precondition, not a demonstrated production defect.
The initial scoped child diagnostic c727 run is superseded by this changed
source before any runtime acceptance; the directory failure remains open.
Actual scoped40519578648/c727 and40519977200/4346 both enter the ordered
child and exit41, with successful inherited-directory fchdir. The latter
also proves both real no-console ABI controls after DETACHED_PROCESS.
The original full's127 is not reproduced or explained by these narrower
runs. Root found another fixture defect: --ordered opens input.bin before
checking its parent's closed descriptor, allowing that open to reuse the
same number. The corrected caller checks closure first, independently reads
both inputs, writes the actual PID regardless of other assertion results
and prints each original assertion's result. No assertion is removed. The
next scoped run builds fresh NDK callers in its own workflow, retaining
immutable ARM support from40502225202 and fresh host binaries.
Scoped40520772723/9b9f625b passes all four jobs. Root independently read
host/NDK build logs and both actual Windows logs: each has all four mandatory
test77 ABI/action/no-console PASS lines and0 test(s) failed. The ordered
caller now requires every independently recorded argv/closure/data/PID
result, as well as actual42 and the returned PID matching the file.
This validates the focused caller after both fixture corrections, not a
cause for the old full127. Current integration3577b330 (docs above accepted
Net4bc6) is merged before the next full run. No runtime/roadmap acceptance
is claimed until both full suites, package and both e2e jobs pass.

## Resumed relative-directory fixture correction

The resumed source resolves the actual self-program with Resolve-Path before
NoDrive. This preserves POSIX lookup after child directory actions while
removing the full driver's accidental relative executable path. Every
original action, PID, isolation, failure and no-console assertion is retained.
The scoped driver now runs the entire unchanged assertion body with both an
absolute Dir and relative Dir=dist on each Windows host. Only PowerShell
parsing and whitespace checks run locally; CI runtime proof is pending.

## Historical owner-requested stop checkpoint

Work stopped on2026-10-27 for a new coordinating session. Full40521839555
at b4c350b80b5b74ea87b2fe4933b185137b39449a is RED: Windows2022 has2 top
failures (the two ABI action callers);2025 has those two plus3 repeated
Web3 teardown/launch failures. Both actual no-console callers pass. Package
and both e2e jobs have successful conclusions; their actual logs have not
been independently reread by Root for this run.

Root read both completed Windows logs122255368046/122255368845. The new
phase diagnostics identify the remaining action failure precisely:

> program lookup ... errno2 ... path[dist/exec4-spawn-arm64-v8a]
> cwd[D:\a\_temp\gesher-root\...\a-\u05d2\b-\u05e9]

The full driver passes relative Dir=dist; test77 consequently supplies a
relative self-program path, then deliberately changes the child's directory.
POSIX resolves that program after actions, so the lookup correctly fails.
The scoped driver used absolute Dir and passed. Next action: make test77's
actual self-program path absolute with Resolve-Path before NoDrive; retain
the production post-action lookup semantics and every original assertion.
No such fix was applied before this stop. This evidence explains the tested
full's127 without a Unicode-chdir production claim; the ANSI import
hypothesis was already disproved by scoped assembly/binding inspection.

Plain logs: C:\work\gesher-ci-review\exec4-spawn-40521839555-root-test2022.log
and test2025.log. Preserve the strict source/CI evidence and the unrelated
Web3 failures. No retry, new CI dispatch or merge after the stop request.

Spawn attributes/signals/scheduling/IDs, complete fork/image replacement,
console delivery, descriptor limits and remaining Exec4 contracts remain
explicitly open in the aggregate Exec4 brief/ADR0074.
