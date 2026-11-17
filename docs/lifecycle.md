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

## Constructed activity stacks

`Context.startActivities`, `TaskStackBuilder`, and `PendingIntent.getActivities`
retain ordered activity history before scheduling client launches. For an opaque
Root → Middle → Top stack, only Top receives `LaunchActivityItem` and
`ResumeActivityItem`. Root and Middle have no client instance or attached process
merely because they are in the history. Back waits for the foreground activity's
pause, then creates the newly visible parent. A parent made visible under a
translucent top is created paused. Removing a task marks its complete history
before callbacks can expose a parent.
After a member is accepted, updating or removing the separate creator package
does not revoke that target's history. The target's own code generation is still
checked before creating a deferred activity.

Each member's source and task flags control task placement and reuse. A preceding
member becomes the next source only when its UID matches the original caller.
The launch task ID and identity-sharing option apply to the final member;
those options are not copied to every parent. Pending activity tokens
copy the complete intent/type arrays for each send. Fill-in affects only the last
intent; UPDATE_CURRENT replaces the stored prefix without changing an already
captured send. Creator options are retained and merged with send options.
Activity permission checks use creator authority. Explicit task-placement
options require START_TASKS_FROM_RECENTS separately for each options supplier;
the holder does not inherit the creator's permission or conversely.
AOSP's activity-send runtime
error handling is preserved: a rejected start is logged inside capability
dispatch, without granting the target or propagating that start exception to the
holder.

The reference is the repository's pinned Android 16 release:
[ActivityStartController.startActivities](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityStartController.java),
[ActivityStarter](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/ActivityStarter.java),
[SafeActivityOptions.selectiveCloneLaunchOptions](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/wm/SafeActivityOptions.java),
and [PendingIntentRecord.sendInner](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/core/java/com/android/server/am/PendingIntentRecord.java).

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

`TaskStackContractTest` in Windows track 99 exercises generated AIDL Parcel
transport, public TaskStackBuilder/PendingIntent calls, lifecycle transactions,
task flags and snapshots, delayed attachment, reentry, permission denial and
history teardown. `tools/test_task_stack.py` runs the same Java service/contract
paths on a host JVM using the existing clock/Binder-identity native boundary.
Its `--source-ref` option recompiles earlier production sources for a negative
control. The host test does not execute ART, Binder Parcel transport, Activity
callbacks in an APK, or window rendering. `PendingIntentContractTest` separately
checks prefix/type/options snapshots and UPDATE_CURRENT versus an accepted send.

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

- Multi-resume, `moveTaskToBack` minimizing, the single process's relaunch and
  screen orientation: the windows3 track (docs/windows3.md, ADR 0053).
- Under SkiaGL a translucent popup costs a read-back per frame; the dim layer
  dims the whole app window below all its popups.
