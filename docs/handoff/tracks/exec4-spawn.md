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

Spawn attributes/signals/scheduling/IDs, complete fork/image replacement,
console delivery, descriptor limits and remaining Exec4 contracts remain
explicitly open in the aggregate Exec4 brief/ADR0074.
