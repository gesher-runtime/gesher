# 0091: An app window is never larger than its display

Status: proposed (issue #426). Amends the resizing parts of ADR 0029, ADR 0053
and ADR 0088; the display stays the decided 400x800@160 default.

## Context

The display an app sees is Gesher's device profile (ADR 0029): 400x800 at 160
dpi unless `GESHER_DISPLAY`/`--display` says otherwise, turned for a task's
orientation (ADR 0053). A resizable Win32 window had no upper limit, so the user
could resize or maximize a task's window to 1920x941 on that 400x800 display.
The task's configuration then had bounds and app bounds of 1920x941, its max
bounds the 400x800 display, and `Display`/`DisplayInfo` still 400x800.

On a device that state cannot happen: a freeform task's bounds lie within its
display area, which is its configuration's max bounds and the display's size.
The framework relies on it. `ViewConfiguration` (made once per process, per
density and device, from the first context that asks) sets its maximum drawing
cache to 4 bytes for every pixel of the max bounds; `View` does not draw a view
whose software layer or drawing cache would be larger. Issue #426's log shows the
consequence in a cheque scan: the scanner's frame view needed 6293408 bytes
(1672x941) of the 1280000 allowed (4 x 400 x 800) and was silently not drawn.
The same log shows the second form: the task turned to landscape (800x400) and
the user maximized it again to 1920x941.

## Decision

A task's window is never larger than the display, turned as its task is
(`TaskConfiguration.clampToDisplay`):

1. **The Win32 window's limit is the display.** `NativeWindow.setMaxClientSize`
   gives `WM_GETMINMAXINFO` the display's size (turned for the task's
   rotation) as the largest tracking size and the maximized size. Resizing
   stops at it, maximizing gives the window the display's size (or the work
   area, if smaller), and `SetWindowPos` from any program keeps to it. An
   Activity minimum larger than the display gives way to it.
2. **The window session clamps what it lays out and reports**: user resizes
   (`applyResize`, which also brings a larger window back), the size a window
   takes from its task's or app's window (`placeInTask`, `taskBounds`), a task
   shape from the configuration (`syncTaskShape`, which sets the new limit
   before the window takes the turned size), and the frames' bounds (`layout`).
3. **The system server and the override agree**: `resizeTask` keeps the task
   within its display, and `TaskConfiguration.override` never produces bounds or
   app bounds outside its max bounds, whoever asks.

Rotation keeps its rules (ADR 0053): a size the user gave the window goes when
the task turns; the turned window is at most the turned display (800x400 on the
default display), and maximizing it gives 800x400.

## Alternatives

- **A display the size of the Windows monitor or work area** (Android desktop
  windowing: freeform windows within a large display). It keeps desktop
  resizing to the whole monitor, but it changes what every app sees: a
  landscape natural display (or one larger than the monitor to keep the phone's
  portrait rotation), `smallestWidth` and screen layout of a tablet, a
  different `Display.getRotation()` for orientation-locked tasks (ADR 0053's
  per-task rotation, which the camera's orientation relative to the display
  rotation also follows). Under CONTRIBUTING.md a new app-visible default is a
  maintainer decision; it is left as an open question rather than part of a
  bug fix.
- **Growing the display with the window** (a display mode change). Apps would
  see display changes on every resize, and the process's ViewConfiguration
  would keep the drawing cache of the display it was made with, so the views
  would still not be drawn.

## Consequences

- A window is at most 400x800 (portrait tasks) or 800x400 (landscape tasks) on
  the default display; it can be made smaller down to the Activity's minimum
  (220dp by default). A larger window needs a larger display
  (`GESHER_DISPLAY`/`--display`).
- `WindowMetrics` maximum bounds, `Display.getRealSize`, `DisplayInfo`,
  `screenWidthDp`/`screenHeightDp`/`smallestScreenWidthDp` and the drawing
  cache limit are consistent with every window size the user can choose.
- Resizing a window to landscape on the portrait display gives a landscape
  configuration at most 400dp wide (`-land`, not `-w600dp`); tests that resized
  beyond the display (600x700, 640x400, 420x620) now resize within it or check
  that the size is kept within it.

## Verification

`tests/dex-services/gesher/services/WindowDisplayBoundsTest.java` (50-services)
checks, for the issue's 1920x941 and other sizes at all four rotations, the
clamped bounds, the override's bounds within its max bounds, `DisplayInfo`, and
the real `ViewConfiguration` built from a real ContextImpl with the task's
configuration (drawing cache at least 4 bytes per window pixel; the unclamped
1920x941 does not fit). `tests/windows/graphics/WindowChecks.ps1` (34-windows)
resizes a real window to 600x700 and 1920x941 and expects 400x700 and 400x800.
