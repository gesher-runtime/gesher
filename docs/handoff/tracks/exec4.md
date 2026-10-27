# Track brief: exec4

Follow up the scoped exec3 implementation verified in ci 40410489251 at
51bbfac, merged into integration 94ccde1. ADR 0074 is reserved. Planned
branch wip/exec4/worktree C:\work\gesher-exec4; root schedules this after
the active tracks. This is open work, not an accepted compatibility claim.

Read exec.md, ADRs 0060/0057/0011, native3 and services3/binder2 briefs.
All compilation/runtime fixtures go to CI. Keep all existing exec3 assertions,
the shared-property ABI and the original camera/device checks intact.

## Remaining contracts

- Broader shell conformance and a real exec replacement that preserves PID,
  process ownership and signals; the current builtin starts/waits for another
  process. Establish precise supported POSIX behavior with actual programs.
- Native-console interruption with correct control-C/signal/process-group
  behavior for foreground jobs, blocked reads and child pipelines.
- Complete binary events/logd and remaining logcat formats/options, with
  binary framing, filters, clear/follow, ownership and failure tests.
- Remaining executable TLS layouts/opcodes beyond the verified API29 local-
  exec slice, using actual compiled executables across relevant toolchains.
- Real fork-child work before exec and static Linux executable support,
  including descriptor/TLS/thread/lifetime semantics. Spawning a replacement
  or accepting an unimplemented path is not proof of these contracts.
- Property caller access policy and Binder report/poke notifications, in
  coordination with services3's identity/permission model and binder2. The
  same-Windows-account shared area is not an Android isolation boundary.

Native3 owns real umask/file permission behavior and general POSIX ABI. Route
the shell's umask to that implementation once it exists; printing a mask alone
does not close it. Shared-area capacity and Windows account/boot assumptions
must remain explicit until designed and verified, not disappear with a green
shell fixture. Add focused actual ART/ELF/APK tests and reserve test 67 when
implementation starts. Final full CI must include latest integration and all
Windows/package/e2e gates. Follow the common merge/report rules; root merges.

## Status and next steps

Active own-runtime crash triage on wip/exec4, based on a06d930 (verified
integration 94ccde1 code plus documentation). Frozen wip/exec3 is unchanged.
Windows4 diagnostic CI40417632512 at cde1c79: Windows-2022 passes exec3;
Windows-2025's single-process exec3 APK dies with SIGSEGV address -1 after
the successful read-mode popen check, before the posix_spawn file-action
check reports. The existing log lacks RIP/caller context. All prior fully
green exec3 evidence is retained; no camera or Windows4 causality is claimed.

Diagnostic CI40428277638 runs at 610e957. Its watch is
C:\work\gesher-ci-review\exec4-watch-40428277638.log (120-second interval).
It enables the existing capped pre-handler sentinel
record only for the own exec3 APK invocations, with map symbolization and
phase markers around the actual spawn/file-action/stdio operations. Original
assertions and native signal/exit behavior are retained. No new local C/C++
or Java compiler/fixture runs. Broader exec4 contracts remain unimplemented;
no roadmap box is ticked.

Prepared a separate live-environment correction after the frozen diagnostic:
RegisterPosix exported a one-time `_environ` array snapshot, but later host
`_putenv_s`/`_wputenv_s` calls can relocate that array. The export now names the
CRT's live pointer slot, with `environ`/`__environ` sharing that data address.
The launcher keeps its separate initial UTF-8 array and now gives both names
the same address; updates to that initial executable array remain a separate
open contract. No conversion or Unicode semantics were changed.

Test 67 adds a Windows host fixture plus an actual NDK shared library. It
loads the library before narrow/wide host mutations, compares slot identities
before any guest traversal, and uses only the integer address of an old array
as a safe stale-snapshot control. Real ELF calls enumerate the array, query
getenv and pass it to posix_spawn with dup2/close actions; guest setenv,
overwrite=false, putenv and unsetenv are included. Both existing ART APK modes
add host Java Os.setenv mutations after native loading and require the same
real child checks. Original exec2's nine assertions remain intact.

Local checks: diff whitespace, workflow YAML parse and PowerShell parse only.
The candidate is not yet built or runtime-verified. Direct exec.yml dispatch
returned HTTP404 (workflow absent on the default branch); no run was created.
Diagnostic CI40428277638 is retained; the corrected full run will use an
immutable `wip/exec4-live-environ` candidate ref, as authorized by root, to
avoid same-ref cancellation of its still-active reusable posix workflow.
The actual checkout remains on wip/exec4. The takeover diagnostic watch uses
C:\work\gesher-ci-review\exec4-takeover-watch-40428277638.log, session49047,
at interval120. Test67 bounds the entire process tree and pipe completion;
failure codes are retained in Java/native output. No precise crash cause is
claimed from the source defect alone.

Corrected full CI40430496516 is dispatched at exact source
9f250803cd184e05d024733da8296410007de76f on the immutable candidate ref
wip/exec4-live-environ. It includes 1cc12d7's live-export correction and
the bounded-harness follow-up. Watch log:
C:\work\gesher-ci-review\exec4-live-environ-watch-40430496516.log, interval120.
Candidate exec/exec job121946700051 succeeded. Its actual job log confirms
NDK -Werror compilation of the new shared fixture/JNI, Java/dex compilation
and APK packaging. This is build evidence only; host C++/Windows execution,
package and e2e are still pending. Log:
C:\work\gesher-ci-review\exec4-live-environ-exec-121946700051.log.
Candidate framework job121946748227 failed after its Java/APK compilation
at a curl HTTP/2 INTERNAL_ERROR (exit92). Actual log:
C:\work\gesher-ci-review\exec4-live-environ-framework-121946748227.log.
This is a transport failure, not a compiler or runtime result. A selected-job
rerun was rejected as `job ... cannot be rerun` while the full run remains
active; retry failed jobs in this same immutable-source run after it finishes.
Do not dispatch a second candidate or infer any Windows pass from the skipped
dependent jobs. No HTTP403/rate-limit rejection occurred.
Original diagnostic610 remains running independently at CI40428277638.
Root alone merges the final verified source and coordinates latest
integration; no branch was merged here.

Remaining-contract source audit while CI builds (no acceptance claims):
- `shell_execute.inc::ReadByte` polls named pipes but calls synchronous
  ReadFile for console input. The shell/exec spawn code has no console-control
  bridge; both spawn entry points unconditionally use CREATE_NO_WINDOW.
  Native console interruption therefore needs an owned-console fixture,
  blocked-read wakeup and foreground-child/group delivery, preserving the
  existing pipe/mailbox trap tests.
- `logging.cc::Record` stores bounded C-string tag/text in a private header;
  Logcat supports text formats and tag/priority filters. That storage/API
  cannot represent an arbitrary binary event payload or logd reader framing.
  Keep EventLog, binary buffers, reader ownership/failure and remaining
  filter/options tests open; a text logger callback is not their completion.
- Shell `exec` still calls External and waits; native exec outside fork waits
  and exits the old process. The previously documented changed-PID contract,
  real fork/static-executable limits, unimplemented TLS forms and property
  caller/notification dependencies are unchanged. These require their own
  actual runtime evidence after the immediate environment/crash regression.
