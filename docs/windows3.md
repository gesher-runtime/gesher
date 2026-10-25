# Window and activity leftovers (windows3 track)

What the lifecycle, webview2, ime and media tracks left open about apps' windows
and Activities (ADR 0053). See docs/lifecycle.md for the lifecycle the system
server drives and docs/graphics.md for app windows.

## Screen orientation

Gesher's display is a phone's that rotates, one per task: each task is a window
of its own on the desktop, and its display turns for its top Activity's requested
orientation (`screenOrientation`, `setRequestedOrientation`), as
`DisplayRotation.rotationForOrientation` turns a phone with no sensor
(`TaskConfiguration.rotationFor`):

- landscape (`landscape`, `sensorLandscape`, `userLandscape`) on a portrait
  display is `ROTATION_90`, `reverseLandscape` `ROTATION_270`; portrait on a
  landscape display likewise; `sensor*`/`user*` of one kind keep the current
  rotation when it is of that kind; `locked` keeps it; `unspecified`, `user`,
  `sensor`, `fullSensor`, `nosensor` are the natural orientation (no sensor);
  `behind`, and a translucent Activity that asks nothing, leave it to the one
  below;
- the system server keeps each task's rotation
  (`ActivityTaskManagerService.updateRotationLocked`, on start, finish and
  `setRequestedOrientation`) and launches and relaunches the task's Activities
  with the turned display's override (`TaskConfiguration.override(bounds,
  rotation)`): screen size and orientation (so `-land`/`-port` resolve), the
  window configuration's bounds, rotation, display rotation and max bounds, so
  the Activity's `Display.getRotation()`, `getSize`, `getRealSize` and
  `WindowManager.get{Current,Maximum}WindowMetrics()` are the turned display's;
  an Activity that does not handle the change is relaunched, as on a phone;
- the window session gives the Activity's window its task's shape from the
  Activity's configuration (`syncTaskShape`): an 800x400 Win32 window on the
  default 400x800 display;
- gesher-app's single process does the same for its one Activity
  (`GesherAppDriver`: the launch override, `setRequestedOrientation` through
  `ActivityClientHook`).

A size the user gave the window goes when the task turns; a window the user
resizes afterwards keeps the display rotation.

Signal Generator (userLandscape) runs in e2e on the default display.

## The window thread and WebView2

A window's thread waits with `MsgWaitForMultipleObjectsEx(QS_ALLINPUT,
MWMO_INPUTAVAILABLE)` and a 50 ms backstop, then drains with `PeekMessage`,
instead of `GetMessage`: with WebView2's process's input queue attached to the
thread, `GetMessage` slept with a click queued (runtime 131, windows-2022).
37-webview's Features.ps1 no longer posts WM_NULL.

## Multi-resume

As Android 10 and later: the top Activity of each visible task is resumed;
starting an Activity pauses only the one in front in its own task. The top
resumed Activity is the top of the task that came to the front last (started
in, activated, restored) and is not at the back; it gets
`TopResumedActivityChangeItem(true)` once resumed, the one before it
`TopResumedActivityChangeItem(false)` (`onTopResumedActivityChanged`).

## moveTaskToBack

`ActivityClientHook` (installed with the window session) sees an app's
`moveActivityTaskToBack`: when the server moved the task (or, in the single
process, for its own Activity) the task's windows are minimized
(`WindowSession.minimizeTask`, `SW_SHOWMINNOACTIVE`), not hidden; restoring one
from the taskbar moves the task to the front.

## Relaunch in a single process

A configuration change the single process's Activity does not handle relaunches
it as `handleRelaunchActivity` does with the window preserved: onPause, onStop,
onSaveInstanceState, onDestroy (changing configurations, non-configuration
instance kept), a new instance with the same token in the same decor view and
Win32 window, onCreate with the saved state, onStart, onRestoreInstanceState,
onPostCreate, onResume.

## IME insets

Each top-level window advises the touch keyboard's `IFrameworkInputPane`
(`AdviseWithHWND`); its Showing/Hiding rows (and a window move or resize while
it is up) become a `kInputPane` window event with the rows of the client area it
covers; the window session puts an IME `InsetsSource` there for the window and
its popups and tells them (`IWindow.resized`): `WindowInsets.Type.ime()`, and
ViewRootImpl/DecorView do `adjustResize` and `adjustPan`. Tests post the same
message (`kMsgInputPane`, WM_APP+0x43: wparam the keyboard's top screen row,
lparam its bottom) since the runners show no touch keyboard.

## Clipboard

The devsvc track's `clipboard` service is Windows' clipboard both ways (text,
HTML, URIs and intents as text; `AddClipboardFormatListener`), in the single
process too; Ctrl+C/V/X in an EditText go through it. 51-windows3 checks it
with SendInput, Get-Clipboard and Set-Clipboard.

## Tests

tests/windows/tracks/51-windows3.ps1 with tests/apps/winapp (winapp.apk), shown
windows: a single process (MainActivity; LandActivity and PanActivity started by
themselves) and the Gesher device (also multi-resume and LandActivity in
MainActivity's task).

## Status and next steps

- First implementation pushed; CI not yet run on it.

## Remaining gaps

- The IFrameworkInputPane path itself is not exercised in CI (no touch
  keyboard on the runners); the insets path is, through the posted message. The
  IME candidate window is not reported as insets.
- Rotation is per task; an Application context's Display stays natural.
