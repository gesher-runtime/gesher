# The activity and window lifecycle

What an Android app observes of its Activities' lifecycle and of its windows on
Gesher, where a desktop's windows stand in for a phone's screen (lifecycle
track; ADR 0048). See docs/services.md for the system server's activity
manager and docs/graphics.md for app windows and popups (ADR 0029, 0040).

## Visibility: paused, not stopped, under a translucent Activity

The system server (`com.gesher.server.ActivityTaskManagerService`) decides
which activities are visible as Android's `EnsureActivitiesVisibleHelper`
does, per task:

- an activity is visible unless an activity above it in its task occludes it;
  an activity occludes the ones below unless its theme is translucent or
  floating (`windowIsTranslucent`, `windowIsFloating`, `windowSwipeToDismiss`:
  `ActivityInfo.isTranslucentOrFloating`, what `ActivityRecord.occludesParent`
  asks). The server reads the theme from the APK as `AttributeCache` does
  (`ActivityTheme`: the framework's AssetManager over the APK and the system
  resources, the activity's theme, else the application's, else the default
  for its target SDK); `Activity.setTranslucent` changes it
  (`convertToTranslucent`, `convertFromTranslucent`);
- a task moved to the back (its window minimized) shows nothing;
- tasks are windows of their own on the Windows desktop (Android's desktop
  windowing), so an activity of another task never hides this one's.

An activity paused because another came in front is stopped once the new one
is resumed or idle only if it is no longer visible; under a translucent or
floating activity it stays paused, and its window stays shown and drawn. When
an activity becomes visible again without coming to the front (the opaque
activity above a translucent one finished, its task came back), it gets a
`StartActivityItem` (onRestart, onStart), as `makeActiveIfNeeded`.

## Minimize, restore, focus

- Minimizing an Activity's window (`WM_SIZE SIZE_MINIMIZED`: a new
  `WindowEvent::kVisibility`) moves its task to the back
  (`ActivityClient.moveActivityTaskToBack`): the resumed activity is paused,
  then stopped, the other visible ones stopped; ActivityThread saves their
  state (`onSaveInstanceState` after `onStop`, targetSdk 28 and up). The window
  stays minimized (its taskbar button) instead of being hidden as a stopped
  Activity's window otherwise is.
- Restoring it moves the task to the front
  (`IActivityTaskManager.moveTaskToFront`): its top activity is resumed
  (onRestart, onStart, onResume) after the resumed activity of another task is
  paused, the visible ones under it started.
- Activating a task's window by the user (shown windows) moves its task to the
  front too, as a click on a freeform task does on Android's desktop.
- Losing the focus to another window (another Windows program) is
  `onWindowFocusChanged(false)`; the Activity stays resumed, as in Android's
  multi-window mode.
- Closing the window is Back (finish), as before.

gesher-app's single process (no system server for its one Activity) drives the
same callbacks itself (`GesherActivityWindow.stop`/`restart`, from
`WindowMode.show`'s lifecycle handler).

## ACTION_OUTSIDE

A window with `FLAG_WATCH_OUTSIDE_TOUCH` (a `PopupWindow` with
`setOutsideTouchable(true)`) gets an `ACTION_OUTSIDE` MotionEvent where a
press outside it starts (`WindowEvent::kOutside`, routed by the app window's
input route to the watching popups, translated by compat/input to
`AMOTION_EVENT_ACTION_OUTSIDE` with the press's coordinates in the popup's
window, as InputDispatcher gives them to a window of the same app); a popup
that is not touch modal lets the press reach the window under it too.

## Tests

tests/windows/tracks/48-lifecycle.ps1 with tests/apps/lifeapp (lifeapp.apk),
in a single process and on the Gesher device.

## Status and next steps

- ci run 40141213634 (both runners): the single-process lifecycle check
  passed (focus, ACTION_OUTSIDE, minimize/restore, resize with
  onConfigurationChanged and -land/-w600dp); on the device everything but
  the red read-back under the dialog-themed Activity (dimmed: test fixed)
  passed, including the opaque Activity's relaunch on resize with its saved
  state. The composition check showed the CPU popup composed with alpha (the
  sampled corner was the desktop, outside the app window: test now samples
  above the card) and no dim under SkiaGL (now a dim layer window). uiapp's
  device check broke because a resize now relaunches it, as on Android: it
  declares the size changes it handles.
- ci run 40191526766 at 404cd11 (phase4/integration 26aa8eb merged): all
  green on windows-2022 and windows-2025, package and e2e: the single-process
  and device lifecycle checks, and the composition check with the CPU
  pipeline and SkiaGL (the dialog's transparent edge shows the red under it
  dimmed to 0x660000, its card white, the app window dimmed to 0x666666).
- Merged with the locale track (ADR 0049): one configuration mechanism in the
  server (the package's configuration with the task's size override; one diff
  for a locale change and a resize; override configurations carry a growing
  sequence number). ci run 40197536380 at aba87e2 (phase4/integration bf91649
  merged): all green on both runners, package and e2e, including 48-lifecycle,
  49-locale, 39-rtl, 50-services, 34-windows and 36-popups.
- Next: nothing pending for this track beyond the gaps below.

## Remaining gaps

- One resumed activity at a time (no multi-resume): another task's activity
  is paused, visible, until its window is activated (shown windows) or
  restored.
- An app's own `Activity.moveTaskToBack` hides its task's windows instead of
  minimizing them.
- The single process does not relaunch its Activity for configuration changes
  it does not handle (it says so on stdout).
- Under SkiaGL a translucent popup costs a read-back per frame; the dim layer
  dims the whole app window below all its popups.
