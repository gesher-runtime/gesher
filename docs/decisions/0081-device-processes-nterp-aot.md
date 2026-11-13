# 0081 — The device's own processes run nterp and AOT code

**Status:** accepted, 2026-11-13. Extends ADR 0033 and ADR 0054.

## Context
The Gesher device's own processes — the launcher (`com.gesher.app.Launcher`),
the persistent system server it starts (`com.gesher.server.SystemServer`,
which inherits the launcher's runtime options), `pm`/`am` from native code
(`com.gesher.server.RuntimeCommands`, started by gesher-exec) and microG's
add-on verification (`com.gesher.server.MicrogAddons`) — ran with
`-Xint -Xusejit:false`. `-Xint` makes ART interpret only
(`ForceInterpretOnly`): no nterp, and none of the boot image's AOT code, so
everything ran on the switch interpreter, which docs/compiled-code.md
measures at about a fifth of nterp's speed.

The option dates from gesher-app.ps1's first version (97066cbf, 2026-10-23),
when every Gesher process had to interpret: compiled code and nterp reached
`Thread*` through `%gs` (ADR 0033, accepted the next day). ADR 0033 and 0036
made nterp, AOT code and the JIT work; ADR 0054 moved app processes to the
JIT by stripping the device's options in `Dexopt.appVmOptions`, but the
device's processes kept them. No document or commit gives another reason, and
no test depends on the production options (tests start their own devices
with their own command lines).

On a user's device, binder calls into the system server were slow
(IActivityManager about 3 s on average, IPackageManager over 12 s).

## Decision
- The device's processes run with `-Xusejit:false` and without `-Xint`:
  nterp and the boot image's AOT code (gesher-app.jar's extension is compiled
  `speed`, framework.jar `speed-profile`), no JIT
  (`tools/package/gesher-app.ps1`, `gesher-app-microg.ps1`,
  `compat/elf/exec/gesher_tools.cc`).
- No JIT for them: in the measurement below the system server with the JIT
  used no less CPU than with nterp and AOT code (9.6 s against 9.3 s in the
  same scenario), and the JIT adds a code cache, profiling info and a
  dual-mapped executable region to every long-lived launcher and to the
  system server. App processes keep the JIT and the profile saver (ADR 0054).
- `GESHER_APP_EXECUTION=interpret` now adds `-Xint -Xusejit:false` to app
  processes, since the device's options are no longer interpret-only.
  `GESHER_ART_INTERPRET_ONLY=1` still puts every process, the device's
  included, on the switch interpreter.

## Measurement
An isolated device (ADR 0080's harness, its own install copy and
`LOCALAPPDATA`), alternating the old and new options round by round; each
round boots the device cold, installs waze.apk, launches SolitaireCG, Waze
(arm64) and OsmAnd (x86_64) with hidden windows, waits 60 s and uninstalls
Waze. Medians of 4 rounds on a quiet machine (5 rounds on a loaded machine in
brackets):

| | `-Xint` | nterp + AOT |
|---|---|---|
| system server CPU, whole round | 12.7 s [19.5 s] | 8.6 s [9.3 s] |
| system server CPU, waze.apk install | 6.3 s [10.4 s] | 3.6 s [4.1 s] |
| `gesher-app install waze.apk` | 10.6 s [19.2 s] | 8.7 s [13.5 s] |
| `gesher-app list`, device boot included | 2.7 s [6.7 s] | 1.1 s [2.3 s] |
| launch to first frame, SolitaireCG | 1.9 s [7.4 s] | 1.4 s [2.1 s] |
| launch to first frame, Waze, first run after install | 26.7 s | 26.6 s |
| launch to first frame, Waze, installed, cold device (8 rounds) | 5.6 s | 5.2 s |
| system server private working set / commit / peak | 58 / 786 / 283 MB | 53 / 787 / 293 MB |
| all processes: working set / private / commit | 1,675 / 1,203 / 6,777 MB | 1,677 / 1,203 / 6,811 MB |

Waze's and OsmAnd's own start is their app process's work (arm64 translation,
network) and does not change; the system server's CPU drops by about a third
and the calls it serves get faster accordingly. Memory is unchanged: AOT code
is a shared file view of the oat files the processes already map.

## Consequences
- The system server and launchers now run AOT code and nterp, so implicit null
  checks and stack overflow probes fault in them too (handled by
  `fault_signals.cc` since ADR 0033 and 0036, as in every app process since
  ADR 0054).
- gesher-app.jar's code runs AOT only when the boot image's gesher-app
  extension matches the jar (the installer ships both from one build);
  otherwise it runs on nterp.
- `gesher-app --single-process` and `--system-server` runs, which use the
  same options for the app, run it on nterp instead of the switch
  interpreter.
- CI tests keep passing `-Xint` on their own command lines; only the package
  job and e2e run the installed options.
