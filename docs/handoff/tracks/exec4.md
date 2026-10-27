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

Reserved, unstarted. Exec3's accepted scope remains verified; all contracts
above are explicit leftovers. No new roadmap box is ticked for this brief.
