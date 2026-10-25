# 0048. Lifecycle: visibility, minimize, resize, translucent popups

Status: accepted (lifecycle track, Phase 4; verified in ci run 40191526766). The coordinator may renumber.

## Context

ADR 0040 made dialogs, menus and Activities with a translucent or floating
theme owned popup windows of the app window, and left gaps that real apps
observe:

- the system server stopped the Activity under a translucent or dialog-themed
  Activity; Android only pauses it, and it stays visible;
- with SkiaGL (ANGLE's swap chain presents) popups were opaque: a dialog's
  rounded card and shadow showed black corners;
- `FLAG_WATCH_OUTSIDE_TOUCH` windows got a press and release outside them, not
  `ACTION_OUTSIDE`;
- minimizing the window did not stop the Activity; resizing it changed only the
  window's bounds, so resources never resolved `-land` or `-w600dp` and no
  Activity was relaunched or told `onConfigurationChanged`.

## Decision

1. **Visibility is the server's, per task, from the theme.** The system server
   reads each activity's theme from its APK as `AttributeCache` does
   (`ActivityTheme`: the framework's AssetManager over the APK and the system
   resources) and keeps `ActivityRecord.occludesParent`'s answer
   (`ActivityInfo.isTranslucentOrFloating`; `convertToTranslucent` and
   `convertFromTranslucent` change it). An activity is visible unless an
   activity above it in its task occludes it or its task is at the back. A
   paused activity is stopped only once it is not visible; a stopped one that
   becomes visible again without coming to the front gets `StartActivityItem`.
   Tasks are separate windows on the Windows desktop (as in Android's desktop
   windowing), so one task never hides another's activities: an activity in
   another task is paused, not stopped, when a new task comes to the front.
   One activity is resumed at a time (no multi-resume).
2. **The desktop's window actions are task operations.** Minimizing an
   Activity's window is `moveActivityTaskToBack` (pause, stop, save state; the
   window stays minimized rather than hidden while its activities stop),
   restoring it and activating it are `moveTaskToFront`, resizing it is
   `resizeTask`: the window session in the app process asks the system server
   through the framework's own client calls, as a desktop shell would ask the
   window manager. Losing the focus to another program is only
   `onWindowFocusChanged(false)`: the Activity stays resumed, as in Android's
   multi-window mode.
3. **A resized window is a freeform task of its size.** Its configuration
   (`TaskConfiguration`) overrides the display's: screen size in dp, smallest
   width, orientation, screen layout, window bounds. The window session puts it
   in the merged configuration ViewRootImpl gets, so the Activity's activity
   config callback delivers `onConfigurationChanged` for what it handles; the
   server relaunches (`ActivityRelaunchItem`, the window preserved) the
   activities that do not handle the change, filtered by their size buckets
   (`SizeConfigurationBuckets.filterDiff`), now if they are resumed or paused,
   else when they come back. This is the same path as the locale track's
   configuration changes (ADR 0049): an activity's configuration is its
   package's (`LocaleManagerService.configurationFor`) with its task's size
   override, and one diff (`diffPublicOnly`, then the size buckets) decides
   between `ActivityConfigurationChangeItem` and a relaunch, deferred until it
   is resumed. gesher-app's single process has no server for its
   Activity: it gets `onConfigurationChanged` for handled changes; unhandled
   ones are reported, not relaunched.
4. **Translucent popups under SkiaGL are read back into layered windows**,
   and the dim behind a dialog is then a layered black popup window of its own
   under the app window's popups (ANGLE's swap chain cannot be dimmed in the
   app window's paint, as the CPU pipeline's frames are).
   Choices for per-pixel alpha with a GPU renderer on Windows:
   - DirectComposition (`EGL_ANGLE_direct_composition`, a composition swap
     chain with premultiplied alpha on a `WS_EX_NOREDIRECTIONBITMAP` window):
     no read-back, but it needs ANGLE's DirectComposition path in Gesher's
     ANGLE build, the window's style fixed at creation, and PrintWindow does
     not reliably return a composition swap chain's pixels, so CI could not
     read popups back as it does now;
   - a swap chain on a layered window: not supported by DXGI;
   - **a pbuffer read back into the layered window** (chosen): hwui's
     `eglCreateWindowSurface` for a translucent popup (`gesher_window_translucent`)
     returns a handle of Gesher's over a pbuffer of the window's size (made
     again when the size changes, at `eglMakeCurrent`); each swap reads the
     frame back (premultiplied, as Skia draws it) into the window's back buffer
     and the window presents it with `UpdateLayeredWindow`, exactly as the CPU
     pipeline's frames are. One read-back per popup frame (popups are small and
     drawn on change), the same composition, dim and capture paths for both
     pipelines. App windows themselves stay on ANGLE's swap chain.
5. **`ACTION_OUTSIDE` is delivered as such**: a window watching outside
   touches gets a `kOutside` event where a press outside it starts, translated
   to `AMOTION_EVENT_ACTION_OUTSIDE` with the press's coordinates in its window
   (same app, so not zeroed); a popup that is not touch modal lets the press
   reach the window under it as well.

## Consequences

- An Activity under a translucent one keeps drawing (paused, its window
  shown); activities of other tasks stay paused and visible after another task
  comes to the front, resumed when their window is activated (shown windows)
  or restored.
- An app calling `Activity.moveTaskToBack` itself has its task's windows
  hidden (not minimized): nothing restores them but a new launch.
- A drag-resize relaunches a non-handling Activity each time a size bucket is
  crossed, as on Android's desktop.
- Large translucent windows under SkiaGL (a translucent Activity's window is a
  popup) pay a read-back per frame; DirectComposition remains the way to drop
  it if that matters.
