# Track brief: exec3

Current continuation status (2026-10-26): full track remains open. Completed
CI40393019261 at6a3289d passes EVERY exec3 Check on both runners, including the
actual installed pm/am target/receiver/force-stop assertions. Windows-2022 whole
suite0 failed;2025 only nine devices Checks from one device-process post-steps
crash. All build/media/package/both e2e pass, zero unexpected/Puzzles generator
exit0. Preserved record now shows ntdll.dll+0x9a7e7, fault address-1 and UTF16-
like pointer-register values; no caller/cause inferred. A narrow safe stack-word/
access-kind extension is prepared (eight candidate words/512bytes per captured
record, eight records/96 published lines maximum); own-file fatal contracts pass
with exit unchanged. Verified integration117 and media ownership corrections are
combined. The actual-class1000 cancel/flush race passes locally and track58 now
invokes it under ART; that ART invocation is pending. Quote-aware Launcher main
token parsing fixes a Windows4 startup defect and passes13 host cases. The next
combined run retains all real assertions and open umask/policy scope; neither
camera ownership proof nor stack-word candidates establish the ntdll cause.
CI40405306826 is running at87d5ada with that combined source. Watch
C:\work\gesher-ci-review\exec3-watch-40405306826.log at120seconds; no duplicate
own run is active. The source includes the latest integration7826cbe docs atop
the verified117 runtime code.

Earlier continuation evidence:

Completed
CI 40385411337 at 80bf394 passes all build/media/package/both e2e jobs (zero
unexpected), shell57, signals/tools/TLS/readiness, ten host assertions, complete
properties/logcat in standalone and both APK modes, and the single-process APK.
Java callbacks, scratch-root inheritance, serial/wait/wait_any and boot/persist
are now actual ART proof. The installed APK stops at pm's logical-image file
check before Binder; an ISA-layout correction and five native launch-contract
checks are prepared, but real pm/am remain pending. Windows-2025 has ten failed
Checks; 2022 fourteen. Other failures follow post-steps devices crashes at -1
(device both, single2022). Trace capture did trigger; a helper path/filter
mistake hid it. The prepared publication-only correction passes an actual native
fatal-child/PowerShell contract with 300 preceding lines and preserved exit7.
No native cause is claimed. media3_next separately owns camera callbacks work.
Merge 6a3289d incorporates docs-only integration cd0b849. CI 40393019261 now runs
that SHA with the pm image-layout and existing-context publication corrections;
watch C:\work\gesher-ci-review\exec3-watch-40393019261.log at 120 seconds.
No camera callback patch or new native diagnostic was imported. Before final
CI merge any later integration. Keep every real assertion and open
umask/policy scope. See docs/exec.md for complete evidence and earlier runs.
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
