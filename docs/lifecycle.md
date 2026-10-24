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

- Done (not yet CI-verified): server visibility rules, minimize/restore,
  focus, ACTION_OUTSIDE; the test app and track script.
- Next: configuration changes on window resize (onConfigurationChanged vs a
  relaunch per `configChanges`, size and orientation qualifiers from the
  window's size); translucent popups under SkiaGL; ADR 0048.
