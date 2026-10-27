# Exec4: spawn action registration

Root owns wip/exec4 in C:\work\gesher-exec4, ADR0074. Test74 is reserved.
The bounded registration slice is merged/accepted; broader Exec4 stays open.

## Status and next steps

Integratione9859654 is merged. Four production guards implement the exact
pinned[bionic registration contract](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/spawn.cpp):
addopen/addclose/addfchdir_np reject negative fd, and adddup2 rejects either
negative endpoint, returning EBADF without changing errno or appending an
action. A positive unopened fd is deliberately accepted; child execution
errors are a separate contract.

Prepared API34 NDK fixtures build for x86-64/arm64-v8a in exec.yml with
Werror. Each actual ELF process checks15 negative boundary cases including
INT_MIN, then uses the still-valid action list with real posix_spawn and
posix_spawnp children, asserting real PIDs and waitpid exit42. A separate
positive-unopened control list is destroyed without execution. Test74
requires the specific ABI, all concrete results and zero failed tests;
argv0 and actual self-path are explicit in the launcher invocation.

Only source inspection, Git and PowerShell parsing run locally. Initial
scoped dispatch of exec.yml returned HTTP404 because main does not yet
contain this workflow; no run started from that request. A branch/path push
trigger bootstraps the ready scoped fixture build on wip/exec4. Scoped exec
CI must first build the NDK programs and existing Java/APK fixtures.
Scoped exec CI40458517945 at3e9f2e0db86a03514c1a119998f3680647a8a7c6
passed. Root read actual job122037370882: both API34 NDK programs compiled
with Werror, and existing LauncherTailTest/LauncherCommandLineTest passed
under the CI JVM. This is fixture compilation only; the production guards
and actual ELF behavior still require full Windows CI.
Before final full CI merge current integration, including exit-freshness
once accepted, then read all jobs, both Windows zero summaries and actual
test74/legacy exec PASS lines, package and both e2e logs.

Final coherent full40466021634 at160e25e8 now verifies the actual production
guards and both ABI ELF callers: test74 passes on Windows2022 and2025, both
whole suites end0, package23 and both ten-app e2e0. Root independently read
all28 job conclusions and five critical logs. Merged as6297e80a; its code
exactly matches the verified source, with later documentation only.
The source-only/scoped records above remain the chronology, not separate
runtime proof. Child-side ordered actions are the new exec4-spawn/test77
candidate, full40498406783 pending, not part of this registration acceptance.

Remaining: actual child-side ordered open/dup/close/chdir/fchdir failures
with pinned Android child127 semantics, relative-path behavior, attributes,
close-on-exec, full fork, signals/job control and executable environment.
The earlier live-environment stage is independently verified; prior
combined badssl-only red runs stay historical red evidence.
