# Track brief: exec3

Current continuation status (2026-10-26): the full track remains open. Completed
CI 40372171824 at 7ae515f passes all build/package/both e2e jobs (zero unexpected),
57 shell cases, signals/tools/TLS, readiness, native3 Process proc, libartbase/
libdexfile scratch cleanup, standalone Java/NDK logcat and unchanged windows3 UI
assertions. Windows-2025 has three failed Checks from one readonly fixture-name
collision, now corrected without relaxing policy; later Java callbacks and
complete APK/pm/am proof remain blocked. Windows-2022 has those three plus
thirteen devices Checks affected by two actual post-steps SIGSEGV exits at -1.
Cause remains open; a coordinator-authorized bounded pre-handler diagnostic
and devices-only enablement/symbolization are prepared for the next combined CI.
The tree includes integration 634a273 (docs on verified AOT3 oatdump f5ce6f6).
The additional property root inheritance fix passes ten native host checks;
matching Java ProcessBuilder assertions compile and await final CI. Keep all
real assertions and open umask/policy scope. See docs/exec.md for exact evidence.
CI 40382268730 now runs source 4568c71 with that combined correction/diagnostic;
watch C:\work\gesher-ci-review\exec3-watch-40382268730.log at 120 seconds.
The original brief below remains unchanged.

Native process leftovers reported by the exec2 track. The original brief
given to the first agent of this track (2026-10-26), kept verbatim. A
successor reads it together with the "Status and next steps" section in
docs/exec.md on its branch.

You are the "exec3" track agent of the gesher project. Your worktree is
C:\work\gesher-exec (reused) on branch wip/exec3 (cut from
phase4/integration and pushed). Work only there, with absolute paths under
C:\work\gesher-exec (an earlier agent wrote into the coordinator's worktree
with relative paths: check `pwd` before editing).

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/exec.md (its status section), ADR 0035 and ADR 0055,
compat/bionic/process_bionic.cc, compat/elf/exec (gesher_exec.cc,
exec_spawn.cc, gesher_tools.cc), tests/windows/tracks/42-exec.ps1 and
43-exec2.ps1.

GOAL, each verified in CI:
- `sh`: a real shell at /system/bin/sh (mksh as Android ships it, or
  toybox's sh, if the license fits ADR 0001): pipes, redirections,
  variables, quoting, if/for/while, functions, `$(...)`, exit statuses;
  `Runtime.exec("sh -c ...")` and native `system()`/`popen()` go through
  it.
- Verify what exec2 implemented but CI never checked: `Process.destroy()`
  runs the program's SIGTERM handler, `destroyForcibly()` kills; ps, cat,
  kill, sleep.
- logcat: the system server (and single-process mode) sets the log file
  the tool reads, so `logcat -d` (and `-s TAG`, `-v` formats, `-c`) shows
  the app's own log lines as on Android.
- getprop/setprop over a shared property store (the system server's
  properties, as Android's property service), so setprop persists for
  later getprop and `SystemProperties.get` in apps sees it (ro.* stay
  read-only).
- `pm` and `am` basics through the system server (pm list packages, pm
  path, am start -n, am force-stop, am broadcast), as apps and scripts call
  them.
- ELF TLS in the executable itself (local-exec): do it if the loader can
  tell the sites apart (e.g. rewrite after relocation, or recognise the
  local-exec relocation-free form by the executable's PT_TLS); otherwise
  record precisely why not.
- A fork child that does real work before exec (beyond the recorded fd/env
  calls) if feasible, else record why not; static executables (ET_EXEC)
  mapped at their addresses if feasible, else keep ENOEXEC with a reason.
- The on-device variant of 43-exec2's checks (an app process of the Gesher
  device).
Tests in tests/windows/tracks/45-exec3.ps1 (NDK test programs of your own),
both runners, single process and on the Gesher device; 42-exec, 43-exec2,
47-arm64 must stay green; e2e too (Puzzles). Your file domain: the
launcher and gesher-exec, compat/bionic/process_bionic.cc and the shim's
process functions (native2 works in compat/bionic too right now: minimal,
careful edits), the system server's property store and pm/am entry points
(minimal edits), tools/package for /system/bin programs,
tests/windows/tracks/45-exec3.ps1 and its programs, docs/exec.md, ADR
docs/decisions/0060-exec3-shell-and-tools.md (0060 is reserved for you),
the roadmap boxes.

Coordinator notes: CI first; NEVER build AOSP locally. Other tracks run in
parallel on the shared CI (windows3, aot2, surfaces, native2, web3,
media3): trigger CI only when code is ready, cancel superseded runs. Watch
with `gh run watch <id> -R gesher-runtime/gesher --exit-status --interval
120`; on HTTP 403 back off ~10 minutes. Keep a "Status and next steps"
section updated with every meaningful push. Before your final CI run, merge
origin/phase4/integration into your branch. Final message = the report
track-rules.md asks for (run ids, PASS lines, remaining gaps, edits outside
your domain, the verified SHA).
