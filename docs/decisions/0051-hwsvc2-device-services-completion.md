# 0051 — Device services, completed: toast activation, the vibrator, content triggers, sleep, time zones, the legacy camera, a camera app

**Status:** accepted, 2026-10-25 (hwsvc2 track)

## Context
The devsvc ([0038](0038-devsvc-windows-backed-services.md)) and devices
([0043](0043-devices-hardware-apis.md)) tracks left open: clicking a toast after the
Gesher device stopped did nothing, uninstalling a package left its
AppUserModelID's registry key, `getSystemService(VIBRATOR_SERVICE)` was not
checked, content URI job triggers were not watched (and the `content` service had
no observers at all), Windows' sleep and real time zone changes were not
exercised, the legacy `android.hardware.Camera` reported no cameras,
`ACTION_IMAGE_CAPTURE` resolved to nothing, there was no geocoder, and camera
preview into a SurfaceView/TextureView had no surface to go to.

## Decision
- **Toast clicks go through a COM activator of Gesher's**,
  `{C18D9445-A7F0-472E-985F-AC354EBEB660}` (`compat/devsvc/devsvc_activator.c`,
  `INotificationActivationCallback`), which each app's AppUserModelID names as its
  `CustomActivator` (`HKCU\Software\Classes\AppUserModelId\Gesher.App.<package>`,
  written only where the activator is registered). The installer registers it as a
  local server (`HKCU\Software\Classes\CLSID\{...}\LocalServer32`:
  `powershell.exe ... gesher-app.ps1 toast-activated`). The running system server
  serves the class (REGCLS_MULTIPLEUSE); when no Gesher process serves it,
  Windows' notification platform runs the LocalServer32 command, whose launcher
  serves one activation (REGCLS_SINGLEUSE), starts the device and hands it the
  click over the `gesher` service (`TOAST_ACTIVATED`). A protocol (`gesher-app:`)
  was the alternative; the activator keeps one path for clicks with the device
  running or not, gets buttons' arguments, and is what Windows documents for
  unpackaged apps. A click may also come through the toast's Activated event
  while the server runs: the same click within 5 s is taken once.
- **Notifications' intents outlive the system server**, as their toasts do:
  each posted notification's PendingIntents (type, creator, request code, flags,
  intents) are kept in `$ANDROID_DATA/system/notifications/<package>/`
  (`NotificationStore`); a click on a toast an earlier run posted makes the
  PendingIntent again as its creator would and sends it; cancel deletes the file
  (and the toast, also for an earlier run's notification). PendingIntents do not
  survive a reboot on Android either, but there notifications do not survive it
  either; Windows keeps the toasts, so the click must work.
- **Uninstalling a package** (`Installer.uninstall`) calls
  `NotificationManagerService.packageRemoved`: its toasts, stored notifications
  and AppUserModelID key go. Gesher's uninstaller removes every `Gesher.App.*`
  key and the activator's CLSID.
- **`vibrator_manager` is a device without a vibrator** (`VibratorManagerService`):
  no ids, no capabilities, `hasVibrator()` false, the null vibrator; vibrate,
  cancel and haptic feedback are accepted. A gamepad's rumble is an input
  device's vibrator on Android (`InputDevice.getVibratorManager`), not the
  system vibrator, so XInput was not used here.
- **`content` has observers** (`ContentService`): register/unregister and
  `notifyChange` with ContentService's matching (the URI, its descendants
  unless NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS, ancestors registered for
  descendants), each observer once with its URIs, self-notification as asked.
  The job scheduler listens to every change: a job's `TriggerContentUri`s
  collect changed URIs and authorities; it runs after its update delay without
  a new change or its maximum delay after the first (ContentObserverController's
  defaults 10 s and 2 min), with them in its JobParameters, once.
- **Sleep and resume**: PBT_APMSUSPEND makes the device not interactive
  (`isInteractive` false, ACTION_SCREEN_OFF to registered receivers) and holds
  non-wakeup alarms (only _WAKEUP alarms fire, as on a sleeping phone);
  PBT_APMRESUMEAUTOMATIC/RESUMESUSPEND is ACTION_SCREEN_ON and the alarms that
  came due fire (repeating ones with their missed count). If no resume comes
  within 2 minutes of a suspend while the device still runs, Windows did not
  sleep: alarms go on as usual.
- **Time zones**: the clock watch already compared Windows' zone at least once a
  minute; a change now also resets the system server's default zone and has
  every app process do so (`IApplicationThread.updateTimeZone`, as
  ActivityManagerService's UPDATE_TIME_ZONE), before ACTION_TIMEZONE_CHANGED with
  the IANA id. The default zone itself is the runtime's: the posix track's
  `/etc/timezone` follows Windows' zone, so after the reset apps see the new one.
- **The legacy `android.hardware.Camera`** runs over the same devices as Camera2
  in the app's process: `android.hardware.Camera`'s natives
  (`compat/devices/camera_frames.cc`) call `com.gesher.server.LegacyCamera`, which
  reads `WindowsCamera` frames and posts events through `postEventFromNative`, as
  libcameraservice's client does. Parameters describe a fixed-focus camera
  without flash or zoom; previews go to callbacks (NV21/YV12, callback buffers)
  and to a preview surface with a native window.
- **Gesher Camera** (`app/gesher-camera`, `gesher-camera.apk` shipped next to
  dalvikvm.exe) resolves ACTION_IMAGE_CAPTURE: a Camera2 preview with take and
  cancel, the photo written to EXTRA_OUTPUT or returned as a thumbnail (only
  where Bitmaps parcel: Gesher's hwui build has no Bitmap parcel natives, so
  across processes the result is RESULT_OK without `data`; preview frames are
  converted by the app itself, as YuvImage has no natives either). The
  persistent device installs it at its start when there is a camera and removes
  it when there is none (a device without a camera has no camera app).
- **Geocoder stays absent** (`isPresent()` false, IOException):
  Windows.Services.Maps' MapLocationFinder answers InvalidCredentials without a
  Bing Maps key (checked locally and on the runners), and Gesher has no key to
  ship.
- **Preview into SurfaceView/TextureView is not done**: a SurfaceView's Surface
  comes from its own child SurfaceControl and BLASTBufferQueue, which Gesher's
  window code does not back (only the window's own SurfaceControl is tagged with a
  Win32 surface), and TextureView needs SurfaceTexture frames on GL; both are the
  graphics stack's to provide. Frames go to any surface that has a native window.

## Consequences
- A toast clicked from Windows' notification center after the device stopped
  starts Gesher (a console-less PowerShell, then the device) and runs the app's
  intent. CI checks this with the activation Windows' platform performs
  (LocalServer32 run, CoCreateInstance, Activate); an actual click on a toast is
  not automated.
- Network job constraints still use Windows' network list manager directly; they
  move to ConnectivityManager's NetworkRequest once the net track's connectivity
  service is merged.
