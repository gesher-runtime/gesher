# 0038 — Device-state services backed by Windows: alarms, jobs, clipboard, toasts, power

**Status:** accepted, 2026-10-24 (devsvc track)

## Context
The persistent Gesher device ([0026](0026-services-persistent-device.md)) served
`alarm`, `clipboard`, `power` and `notification` as stand-ins (alarms never went
off, the clipboard was the device's own, notifications were logged, wake locks
did nothing), and had no `jobscheduler`, `batteryproperties` or `batterystats`:
WorkManager, sync adapters, reminders, copy and paste with other Windows
programs, and an app's notifications did not work. On Android these services
are system_server's and talk to the kernel (alarm timers, wakelocks), HALs
(battery) and SystemUI (the status bar). On Windows the same roles have
Windows mechanisms: waitable timers, power requests, the clipboard, toast
notifications of an AppUserModelID.

## Decision
- **The services stay in the system server, in Java, with Android's
  semantics** (`com.gesher.server`: `AlarmManagerService`,
  `JobSchedulerService`, `ClipboardService`, `NotificationManagerService`,
  `PowerManagerService`, `BatteryService`), registered in `Services.create`;
  what they need of Windows is one natives class, `WindowsHost`
  (`compat/devsvc`, in libandroid_runtime's builtin library). Delivery goes
  through the device's own paths: a PendingIntent is sent with
  `ActivityManagerService.sendIntentSender` (a broadcast to a manifest receiver
  starts the package's process, `withProcess`), a JobService is bound with
  `bindService`, listeners are called over binder.
- **One hidden top-level window per system server** (a thread of its own)
  receives `WM_CLIPBOARDUPDATE` (AddClipboardFormatListener),
  `WM_TIMECHANGE`, `WM_SETTINGCHANGE` and `WM_POWERBROADCAST`; a Java thread
  waits for them (`WindowsHost.nativeWaitEvents`) and hands them to the
  services. Message-only windows do not get broadcasts, so it is a real,
  never shown window.
- **Alarms wait on a waitable timer at an absolute wall-clock time**: RTC
  alarms follow clock changes, an alarm whose time passed during sleep fires
  on resume, and a `_WAKEUP` alarm sets the timer's resume flag (Windows wakes
  if wake timers are allowed). ELAPSED alarms are converted at each wait.
  `WM_TIMECHANGE` (or a jump of the wall clock against the monotonic clock)
  is `TIME_SET`; a new Windows time zone (`GetDynamicTimeZoneInformation`,
  mapped to an Olson id by ICU's windowsZones) is `TIMEZONE_CHANGED`; local
  midnight is `DATE_CHANGED` (registered receivers only, as Android 8's
  implicit broadcast limits). TIME_SET, TIMEZONE_CHANGED and
  NEXT_ALARM_CLOCK_CHANGED go to each package's registered and manifest
  receivers.
- **When the device is not running, nothing fires** (no Windows scheduled
  task): alarms and jobs live in the device as they live in a phone's RAM;
  scheduled work due within the idle timeout keeps the device from stopping
  (`DeviceWork`), later work is lost when it stops, as at a reboot, and
  persisted jobs (`JobInfo.isPersisted`) are written to
  `$ANDROID_DATA/system/jobs` and scheduled again at the next start. A
  scheduled task would start a JVM device at arbitrary times on the user's PC
  and leave Windows state behind after an uninstall; apps already expect to
  reschedule after a reboot.
- **Jobs** follow JobStatus' times (latency, deadline, periodic windows with
  flex, linear/exponential backoff) and constraints from Windows: network from
  the network list manager (`INetworkListManager`, unmetered from
  `INetworkCostManager`), charging from AC power (a PC without a battery is
  charging), battery-not-low from the battery flag, device idle from
  `GetLastInputInfo` (30 minutes without input), storage never low; content
  URI triggers are not watched. Up to 4 jobs run at once, each stopped after
  10 minutes. `GESHER_JOBS_TIME_SCALE` divides periods for tests.
- **The clipboard is Windows' clipboard**: an app's clip is written as
  CF_UNICODETEXT (and CF_HTML when an item has HTML); a change by any other
  program becomes the primary clip and is told to the apps'
  OnPrimaryClipChangedListeners. Our own writes are recognized by the
  clipboard sequence number.
- **Notifications are toasts of the app's AppUserModelID**
  (`Gesher.App.<package>`, ADR 0028), shown by the system server through
  WinRT (`ToastNotificationManager.CreateToastNotifierWithId`), tagged with
  the notification's id and tag so that an update replaces and cancel removes
  (`History.RemoveGroupedTagWithId`). The id is also registered under
  `HKCU\Software\Classes\AppUserModelId\<id>` with the app's label and icon (a
  PNG drawn by the shell track's `AppIcon`), so toasts are named and work
  without a Start menu shortcut. Channel importance maps to how it shows; a
  click while the device runs (the toast's Activated event) sends the content
  intent, a button its action's. The WinRT code is C (`devsvc_toast.c`):
  mingw-w64's C++ WinRT headers do not compile (IReference<BYTE> and
  IReference<boolean> collide), their C form does; combase's functions are
  looked up at run time, so no WinRT import library is linked.
- **Wake locks are a power request** (`PowerCreateRequest`/`PowerSetRequest`,
  `PowerRequestSystemRequired`, `PowerRequestDisplayRequired` for screen
  locks) whose reason names the holders: users see them in
  `powercfg /requests`, and it is process-wide, unlike
  `SetThreadExecutionState`, which binds the state to the calling thread.
- **The battery** is `GetSystemPowerStatus`: a sticky ACTION_BATTERY_CHANGED
  (sticky broadcasts of the system, `StickyBroadcasts`, returned and delivered
  by `registerReceiver`), `batteryproperties` and `batterystats` for
  BatteryManager; battery saver is Windows'.

## Consequences
- Alarms, jobs (and so WorkManager), copy/paste and notifications behave as
  apps expect while the Gesher device runs; Windows shows the app's
  notifications in its notification center with the app's name and icon.
- The system server has a hidden window and joins COM's multithreaded
  apartment on the threads that call WinRT.
- Clicking a toast after the device stopped does nothing (no COM activator
  or protocol is registered for Gesher apps); a later step could register a
  `gesher-app:` protocol in the installer and use protocol activation.
- Not covered: content URI job triggers, app standby buckets and doze, exact
  alarm permission (granted to all), `setTime`/`setTimeZone` from apps
  (refused: Windows owns the clock), app processes' default time zone (the
  posix track's `persist.sys.timezone` handling decides it).
