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
inside that actual process. Both-ABI fixtures and CI are still pending;
the code has not been compiled locally. No roadmap/runtime claim is closed.

Spawn attributes/signals/scheduling/IDs, complete fork/image replacement,
console delivery, descriptor limits and remaining Exec4 contracts remain
explicitly open in the aggregate Exec4 brief/ADR0074.
