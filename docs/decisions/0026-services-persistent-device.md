# 0026 — A persistent Gesher device: one system server, installed packages, processes on demand

**Status:** accepted, 2026-10-23 (services track)

## Context
Until now ([0019](0019-services-system-server.md), [0024](0024-services-app-process.md),
[0025](0025-services-app-components.md)) `gesher-app my.apk --system-server` started
binderd, servicemanager and a system server for one run, gave the server the APK on
its command line, ran the app in the launcher's own process, and stopped all three
at the end. On Android the system server outlives apps: packages are installed once,
the launcher asks the system server to start an activity, and the system server has
the zygote start the app's process when it needs one, for an activity, a service, a
broadcast or a provider, also for another app.

## Decision
- **A Gesher device per user**: binderd, servicemanager and a system server
  (`SystemServer --persistent`) shared by every launch. The first launcher
  (`com.gesher.app.Launcher`, what `gesher-app` runs) starts them, with a binder device
  name of their own, **detached**: no console window, a process group of their own,
  stdout and stderr appended to log files, and no handle of the launcher's (Win32
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), asking to break away from the launcher's job.
  So they outlive the console, and a test runner reading the launcher's pipes is not
  held by them. Natives: `com.gesher.server.Zygote`
  (`compat/framework/services_zygote.cc`, registered by libandroid_runtime).
- **Finding it**: `$ANDROID_DATA/system/gesher-device` names the device and the pid
  and creation time of each process; `gesher-device.ready` holds the system
  server's pid once its services are registered. Launchers read them under a named
  mutex (`Local\gesher-<hash of $ANDROID_DATA>-device`), so two launches at once start
  one device. A device whose system server is gone (or never gets ready) is stale:
  the next launcher ends what is left of it and starts a new one.
- **Stopping**: `gesher-app shutdown` sets a named event the system server waits on
  (the PowerShell launcher sets it without starting a runtime); the system server
  also stops after `GESHER_IDLE_TIMEOUT` seconds (default 600) with no app process
  and no launcher call. It ends its app processes, removes its state and exits. It
  puts itself, binderd, servicemanager and (as its children) every app process in a
  **kill-on-close job**, so whatever way it ends, crash included, the device ends with
  it. The uninstaller and the installer (before an update) run `gesher-app shutdown`.
- **Installed packages**, as PackageManagerService keeps them: `install` copies the APK
  to `$ANDROID_DATA/app/<package>-<n>/base.apk`, extracts `lib/x86_64` next to it (the
  package's nativeLibraryDir), keeps `data/<package>` as its data directory and lists it
  in `system/packages.list`, which the system server reads at start (parsing each
  package with PackageParser2, as at boot). The same APK again changes nothing; another
  APK for the package is an update (`<n>+1`, the package's processes stopped first).
  `uninstall` removes code, data and entry. The package manager's map is replaced as a
  whole on a change, so binder threads read a consistent one. A control service of
  Gesher's own, `gesher` (a Binder with hand-written Parcels: install, uninstall,
  status, shutdown, a package's last exit code), carries what Android does with
  PackageInstaller sessions and `adb`; listing is the framework's
  `IPackageManager.getInstalledPackages`/`getPackageInfo`.
- **Processes on demand (a zygote, lite)**: an activity start, a service, a broadcast or
  a provider for a package with no process makes `ActivityTaskManagerService` start one,
  as ProcessList asks the zygote: a new dalvikvm with the system server's own runtime
  options (its command line up to the main class) running
  `android.app.GesherAppProcess --package P --start-seq N --target-sdk S`, its output
  appended to `logs/<package>.log`. The process attaches with `startSeq`, which pairs it
  with the package (`attachApplication`), and is bound. A process that dies before it
  attaches fails what waited for it. Processes that start by themselves and offer
  themselves (`gesher-app --system-server`) keep working: a start asked by such a
  process goes to it (the self-attached launcher now attaches before it asks).
- **When a process ends**: it is told to exit (`scheduleExit`) when it has no activity,
  no service (started or bound), no work waiting for it, and no client process of
  another package holding one of its providers (ContentProviderConnection, lite: the
  clients are processes, dropped when they die). A process told to exit hands out no
  provider any more, so a later client starts a new one. A spawned process also exits
  if the system server dies.
- **Providers of another package**: `getContentProvider` for a provider whose process
  does not run starts it and waits (up to 120 s) for it to publish the provider, as
  ContentProviderHelper waits for a launching provider.
- **The launch path**: `gesher-app my.apk` (and the `.apk` association) is now
  install-or-update, then `startActivityAndWait` from the launcher (no caller process),
  which makes the system server start the app's process; the launcher copies the app's
  log to the console, removes the task once the activity is idle (headless; `--keep-task`
  lets the app finish by itself), waits for the app's process to end and returns its
  exit code. **This is the default now**; `--single-process` keeps the single-process
  driver (stand-in services in the app's process), `--system-server` the device of one
  run.

## Consequences
- Every app runs the framework's app-process path, as on a device; apps can start each
  other's activities and use each other's providers and services.
- A launch costs one dalvikvm boot for the launcher and one for the app (plus the system
  server's once); later launches reuse the system server.
- The device's processes keep running between launches until shutdown or the idle
  timeout; the installer stops them.
- Default-mode limits are the attach path's: headless stand-ins until the graphics
  track's window session, and no system fonts, so an app whose theme has an action bar
  stops measuring text (`--single-process` does not create the decor view).
- Cross-process cursors need CursorWindow's natives (not registered yet), so a
  provider of another package answers `insert`, `getType`, `call`..., not `query`.
- All app processes still share one uid; no permission or `exported` checks.
