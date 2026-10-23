# 0018 — The app process attached to the system server, on the framework's client path

**Status:** accepted, 2026-10-23 (services track)

## Context
With a system server of Gesher's own ([0017](0017-services-system-server.md)),
gesher-app still drove the app itself (`GesherAppDriver`, a scaled-down
`handleBindApplication` and `performLaunchActivity`). On Android the app
process is the framework's: `ActivityThread.main` attaches it to
ActivityManagerService, which binds it (`bindApplication`) and then drives
each Activity with `ClientTransaction`s that `TransactionExecutor` runs on the
main thread. The real system services (services.jar) are still out of reach.

## Decision
- **The client side is the framework's, unchanged**: in
  `gesher-app --system-server` the app process runs `new ActivityThread()`,
  `attach(false, 0)` (`android.app.GesherAppProcess`), and `Looper.loop()`;
  `handleBindApplication`, `handleLaunchActivity`, `handleStartActivity`,
  `handleResumeActivity`, `handlePauseActivity`, `handleStopActivity` and
  `handleDestroyActivity` are framework.jar's own, reached through
  `IApplicationThread.bindApplication` and `scheduleTransaction`.
- **The server side is Gesher's** (`com.gesher.server`):
  `ActivityManagerService` (attachApplication, finishAttachApplication,
  bindApplication's arguments), `ActivityTaskManagerService` (process and
  activity records, tasks, startActivity/startActivityAndWait, removeTask,
  getTasks, the transactions: Launch + Resume, then Pause, Stop, Destroy
  one after the other, each after the app's report) and
  `ActivityClientController` (the app's reports). With no zygote, a process
  is started by the launcher and offers itself; a start for a package with
  no process takes the process that asked for it, else the first unassigned
  one. A process whose last activity is destroyed gets `scheduleExit`, as AMS
  kills the process of a removed task.
- **gesher-app plays the launcher too**: a thread of its own calls
  `startActivityAndWait` (`am start -W`) and, once the activity is idle,
  `removeTask` (the user closing the task), so a headless run ends.
- **What the path needs below Java is provided, not avoided**:
  - binder carries file descriptors (BINDER_TYPE_FD: the fd's Windows handle,
    duplicated by binderd into the receiver), as `bindApplication` sends the
    ApplicationSharedMemory region;
  - libandroid_runtime registers frameworks/base's own ApplicationSharedMemory
    and PropertyInvalidatedCache natives (their sources included in Gesher
    files that register them through GESHER_JNI), ClassLoaderFactory's
    `createClassloaderNamespace` (no linker namespaces: success),
    ActivityThread's and DebugStore's natives (no-ops);
  - libutils' `systemTime` reads `clock_gettime` (system/core patch 0006), so
    `SystemClock.uptimeMillis` and `System.nanoTime` share the monotonic
    clock, as Choreographer needs;
  - the system server's binder calls carry SYSTEM_UID
    (`GESHER_BINDER_UID`), as ApplicationThread expects of its callers.
- **Graphics and view natives are stand-ins until the graphics track's**:
  `compat/framework/attach_standins.cc`, generated from frameworks/base's JNI
  tables (`gen_attach_standins.py`) and registered before the graphics rows,
  so the real natives replace them when linked. The window session is
  `StandInWindowSession` (ADD_OKAY, the display's frames, no surface), which
  `WindowManagerService.setSessionFactory` replaces. Choreographer times
  frames with messages (`debug.choreographer.vsync=false`) while there is no
  vsync source.
- **Classes framework.jar is compiled against but lacks** (mainline modules)
  get placeholders in gesher-app.jar, as before: TrafficStats,
  SdkSandboxActivityAuthority/ActivityContextInfo, StatsEvent/StatsLog.
- **Opt-in still**: the single-process mode with `GesherAppDriver` stays the
  default (`gesher-app.cmd`, the `.apk` association); `--system-server`
  selects the framework's path.

## Consequences
- An app's lifecycle in `--system-server` mode is the framework's, so what
  works there works as on a device, and what is missing shows up as the
  framework would meet it (each step above was found that way).
- The window path is exercised headless (the decor view is added, laid out
  and removed through the session) before anything is drawn; the graphics
  track plugs a real session and real natives into the same path.
- Workarounds kept until other tracks land: the empty `sans-serif` typeface
  when there is no fonts.xml (fonts are the graphics track's), the stand-in
  natives, vsync by messages.
- The system server answers every call lifecycle.apk makes; the stand-in log
  (`stand-in IFoo.bar not implemented`) lists what other apps reach next.
