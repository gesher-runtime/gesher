# Exec4 recovery checkpoint

- Only write checkout: `C:\work\gesher-exec4`, branch `wip/exec4`.
- Verified live-environment source: `9f250803cd184e05d024733da8296410007de76f`.
  Full CI40430496516 attempt2: all 28 jobs complete (22 success/six skips),
  both Windows zero failures, 11 exec3 Checks/57 shell cases, new test67,
  both host-mutated APK modes, actual package and both required e2e pass.
  Root independently read all final proof. Attempt1 failed only framework
  curl92; retry occurred after completion on the identical source.
- Frozen refs: `wip/exec4-live-environ` = `9f25080`;
  `wip/exec4-live-environ-reviewed` = docs-only `76b8d5c`.
- Final combined source: `b42922049201d5dc7ef1c1ca61cd7fb2db404220`.
  This cleanly merges integration630c20c (root-verified code42cc1c0,
  CI40432541082) and its real startup profile/checks into the verified slice.
  Native3 width/CPU and monotonic-clock candidates remain outside it.
- Final combined full CI40438421809 is pending. Pre-checkpoint docs head is
  `b64721a`; this checkpoint changes documentation only. Root will monitor
  final acceptance during the temporary agent-slot handoff. Do not change
  source, move frozen refs, dispatch another CI or merge integration/main.
- Leave watcher running: `gh run watch 40438421809 -R gesher-runtime/gesher
  --exit-status --interval 120`, session28115. Watch file:
  `C:\work\gesher-ci-review\exec4-final-combined-watch-40438421809.log`.
- Previous verified execution logs under `C:\work\gesher-ci-review`:
  `exec4-live-environ-attempt2-2025-121960683003.log`,
  `exec4-live-environ-attempt2-2022-121960683017.log`,
  `exec4-live-environ-attempt2-package-121960683093.log`, and
  `exec4-live-environ-attempt2-e2e-{2022-121961593664,2025-121961593810}.log`.
- Diagnostic610 / CI40428277638 did not reproduce the crash: both APK modes
  pass, all 13 spawn/stdio phases complete, no sentinel record. Windows2022
  has zero failures; Windows2025 has two external expired.badssl timeouts.
  Package/both e2e pass. Preserve `exec4-diagnostic-*.log` and the original
  `windows4-full-2025-121919469507.log`; exact older crash causality is open.

Next-source spawn audit is separate and unimplemented. DoPosixSpawn currently
swallows failed open/missing dup2 sources and records fchdir without executing
it. The exact pinned [bionic source](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/spawn.cpp)
was inspected as text: negative fd action registration returns EBADF; action
failures in the child terminate it with127 while posix_spawn returns0/a real
child pid. Do not substitute glibc-style synchronous errors for that Android
contract. Prepare actual NDK/API34 action, directory, close-on-exec and failure
tests before implementing the next slice. No next-source code was applied.

No local C/C++/Java compilation or native/JVM fixture run. Only source/script
inspection, formatting, lightweight parsing, git and gh log/watch operations.
The only authored shared build edit is a small CMakeLists.txt test target;
no authored ci.yml, production Main.java, Unicode or clock changes. Root
alone owns integration/main/PR/releases. Remaining exec4 contracts are still
open in ADR0074 and tracks/exec4.md.
