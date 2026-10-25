# Track brief: exec2

Native process leftovers reported by the exec and posix tracks. The original
brief given to the first agent of this track (2026-10-25), kept verbatim. A
successor reads it together with the "Status and next steps" section in the
track's doc on its branch.

You are the "exec2" track agent of the gesher project. Your worktree is
C:\work\gesher-exec (reused) on branch wip/exec2 (cut from phase4/integration
and pushed). Work only there.

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/exec.md (its "Not done / limits" list is your goal), ADR 0035,
docs/posix.md and ADR 0045, docs/native-libs.md (the bionic shim's gaps),
docs/arm64.md (the arm64 track will run arm64 executables through your
launcher: keep its hook points), compat/posix/src/signal.cc.

GOAL: native processes behave as on Android, each verified in CI:
- `fork()`+`exec*()`, `posix_spawn`, `system()` and `popen()` from native
  code (an app's `.so` or an ELF executable): decide how (fork+exec pairs
  mapped to a spawn; `vfork`-style; a real `fork` is out: record it in an
  ADR), with fd inheritance (O_CLOEXEC, dup2 redirections), environment,
  working directory, `waitpid`/`wait4` with the 128+signal decoding the posix
  track uses.
- Signals between processes: `kill(pid, sig)` to other Gesher processes
  (SIGTERM, SIGKILL, SIGINT, SIGUSR1/2, SIGHUP; signal 0 for existence),
  delivered to the target's handlers; `Process.destroy()` runs the program's
  SIGTERM handler, `destroyForcibly()` kills.
- ELF TLS in the executable itself (local-exec `%fs:-N` forms that trap
  today).
- The programs Android has in /system/bin that apps really call: `sh` as a
  fuller shell (pipes, redirections, variables, `if`/`for`, quoting: toybox's
  or mksh's, if the license fits ADR 0001), `getprop`/`setprop`, `logcat`
  (reading Gesher's log), `ls`, `cat`, `id`, `ps`, `pm`/`am` basics as
  toybox/AOSP provide them; `su` stays absent (no root).
- Static executables (`ET_EXEC`, no PT_INTERP) if the loader can map them at
  their addresses; otherwise record why not.

Tests in tests/windows/tracks/43-exec2.ps1 (NDK test programs of your own),
both runners, single process and on the Gesher device; e2e must stay green
(Puzzles uses exec). Your file domain: the launcher and gesher-exec,
compat/posix process/signal code (shared with posix: minimal, careful edits),
the shim's process functions, tools/package for any new /system/bin programs,
tests/windows/tracks/43-exec2.ps1 and its test programs, docs/exec.md, ADR
docs/decisions/0055-exec2-native-processes.md (0055 is reserved for you), the
roadmap boxes.

Coordinator notes: other tracks run in parallel on the shared CI (media,
arm64, hwsvc2, windows3, aot2): trigger CI only when code is ready, cancel
superseded runs. Watch with `gh run watch <id> -R gesher-runtime/gesher
--exit-status --interval 120`; on HTTP 403 back off ~10 minutes. Keep a
"Status and next steps" section updated with every meaningful push. Before
your final CI run, merge origin/phase4/integration into your branch. Final
message = the report track-rules.md asks for (run ids, PASS lines, remaining
gaps, edits outside your domain, the verified SHA).
