# ADR 0098: App processes enter through ZygoteInit and RuntimeInit

Status: accepted (maintainer decision, 2026-11-25)

## Context

On Android an app process is a fork of the zygote. The child returns from
`ZygoteServer.runSelectLoop` inside `ZygoteInit.main`, runs
`ZygoteConnection.handleChildProc` (`ZygoteInit.zygoteInit`, then
`RuntimeInit.applicationInit`, whose `findStaticMain` gives a
`RuntimeInit.MethodAndArgsCaller`), and `ZygoteInit.main` runs that caller:
`ActivityThread.main` through `Method.invoke`. Every app's main thread therefore
ends in these frames:

    at android.app.ActivityThread.main(ActivityThread.java)
    at java.lang.reflect.Method.invoke(Native Method)
    at com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run(RuntimeInit.java)
    at com.android.internal.os.ZygoteInit.main(ZygoteInit.java)

Windows has no fork, so Gesher's device zygote (`com.gesher.server.Zygote`)
starts each app process as a new runtime (ADR 0024). Until now that runtime's
main class was `android.app.GesherAppProcess`, which did `ActivityThread.main`'s
steps itself, so the main thread ended in
`android.app.GesherAppProcess.run`/`main`. Apps can see their own stack
(`Throwable.getStackTrace`, `Thread.getStackTrace`), and some check that it is
an app process's. Google Play's PairIP (`libpairipcore.so`, in Spoke) reads
every frame of a new `Exception`'s stack before it decides how to go on; an
unknown process entry is what a hooking framework's entry (Xposed's) looks like.

## Decision

An app process runs the real entry path; only the fork is replaced.

- The device zygote starts `com.android.internal.os.ZygoteInit` with
  `--gesher-app-process --package P --start-seq N --target-sdk S
  --runtime-flags F` (SystemServer).
- `patches/frameworks/base/0790-zygoteinit-gesher-app-process.patch`:
  `ZygoteInit.main` with that first argument does what the forked child does
  from `runSelectLoop`'s return on. `android.app.GesherAppProcess.specializeChild`
  (on the boot class path after framework.jar) does the zygote's and the child's
  specialization that Gesher does itself and returns the child's command (the
  target SDK, `android.app.ActivityThread`, `seq=N`); then
  `RuntimeInit.applicationInit` gives the caller and `ZygoteInit.main` runs it.
  `ActivityThread.main` is AOSP's own: it prepares the main Looper, sets the
  mainline modules' service managers, attaches with the start sequence number
  and loops.
- `specializeChild` keeps what the process did before: the log redaction, the
  zygote's specialization (`ProcessSpecialization`, dumpable unless debuggable,
  ADR 0093), the method trace, the runtime bootstrap (without the main Looper
  and the mainline service managers, which are `ActivityThread.main`'s now), the
  taskbar identity, the system server's death watch, the binder threads, the
  display, cameras, accessibility, the proxy, fonts, frame timing and the
  windows.
- The process follows its ActivityThread's messages with a `Looper.Observer`
  (Android's hook for message dispatch): after `BIND_APPLICATION` it tracks the
  Activities (for the camera's orientation), and when `EXIT_APPLICATION`'s
  `Looper.quit()` is refused (the main Looper may not quit) it ends with 0, where
  a device's server would kill it. A failure that leaves the main thread ends
  the process with 1 after the crash notice, through the default uncaught
  exception handler (an app's own handler that hands on reaches it too).
- No frame is added, removed or renamed: the stack is the code that runs.

## Consequences

- An app's main thread shows Android's process entry. Line numbers are those of
  Gesher's framework build.
- `ZygoteInit.zygoteInit`'s `RuntimeInit.redirectLogStreams` and `commonInit`
  and `nativeZygoteInit` are not run: System.out and System.err stay the
  package's log file, the runtime's handlers and binder threads are Gesher's
  (bootstrap and `specializeChild`), as before.
- `RuntimeInit.nativeSetExitWithoutCleanup` is registered (`services_natives.cc`)
  and recorded only: Gesher's runtime has no VM exit hook, and `System.exit`
  keeps the C runtime's exit, whose cleanup releases Windows resources.
- `ZygoteInit.java` joins frameworks/base's sparse checkout (`aosp.lock`).
- The launcher's in-process path (`gesher-app` without a device,
  `GesherAppProcess.run`) is unchanged.
