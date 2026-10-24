# 0029. Graphics: an app's windows are drawn by its own process

Status: accepted (graphics track, Phase 3; verified in ci run 40048707298). The coordinator may renumber.

## Context

On Android an app's `ViewRootImpl` asks the window manager (system_server's
`Session`, an `IWindowSession`) to add its window (`addToDisplay`) and lay it
out (`relayout`); the window manager answers with frames, configuration and
insets, a `SurfaceControl` for a SurfaceFlinger layer, and an `InputChannel`.
The app draws into the layer's buffers (BLASTBufferQueue) and SurfaceFlinger
composes them.

Gesher has no compositor: each Android window is a top-level Win32 window
(ADR 0021), and on Windows a window belongs to the thread and process that
created it; drawing into another process's window would mean shared memory and
cross-process presents for every frame. The Gesher device (ADR 0026) runs the
system server and each app in processes of their own, and its `SurfaceControl`,
`BLASTBufferQueue` and `Surface` parcel natives are stand-ins (no handles cross
processes). `WindowManagerService.setSessionFactory` lets a real session plug
in; the question is which process serves it.

## Decision

- **The window session runs in the app's process**:
  `com.gesher.app.window.WindowSession` (an `IWindowSession.Stub`) is
  `WindowManagerGlobal`'s session in every process that has windows: set
  before the app attaches (`WindowMode.install`), so ViewRootImpl never asks
  the system server's `openSession`. It is the same session in both modes:
  `gesher-app --single-process --window` (everything in one process) and an
  app process of the Gesher device (`GesherAppProcess`, started by the system
  server). In-process, `WindowManagerService.setSessionFactory` hands it out
  as well.
- **`addToDisplay`** records the window (frames, configuration and insets on
  DeviceProfile's display), and opens its input channel pair
  (`InputChannel.openInputChannelPair`: the client's end to ViewRootImpl, the
  other kept for the window's input; empty with the stand-in natives).
  **`relayout`** creates or resizes the Win32 window (in this process) and its
  `Win32Surface`, and hands out a stand-in `SurfaceControl` tagged with that
  window: ViewRootImpl's `BLASTBufferQueue` hands out the window's `Surface`
  (`BLASTBufferQueue.nativeGetSurface`'s provider, window_jni.cc). hwui's
  renderer draws into it with the CPU pipeline, which queues each frame
  (frameworks/base patch 0103), so frames show without anything else
  presenting them. **`finishDrawing`** shows the window (hidden windows are shown
  off the desktop: a tool window left of the virtual screen, not activated,
  since PrintWindow reads nothing back from a window that was never shown),
  **`remove`** closes it.
- **The window**: titled with the Activity's label (the window's accessibility
  title, else its title), with the process's AppUserModelID set on the window
  too (`SHGetPropertyStoreForWindow`, from `$GESHER_APP_USER_MODEL_ID`, which
  the system server gives each package's processes; ADR 0028), and the
  package's Start menu icon (`<data>/gesher-shell`, from the shortcut record)
  as its icon.
- **Closing the window** (close button, Alt+F4, `WM_CLOSE`) calls
  `Activity.finish()`: on the device the system server pauses, stops and
  destroys the activity with client transactions, ViewRootImpl removes the
  window (`remove`), and with no activity left the server ends the process
  (`scheduleExit`). In a single process, gesher-app's driver does the same
  steps itself.
- **Window mode per launch**: `gesher-app` (the Launcher) takes `--window`
  (the default, also for a double-clicked .apk), `--hidden-window` (off the
  desktop: CI reads windows back with PrintWindow), or `--headless` (no Win32
  windows: the system server's `StandInWindowSession`, no surface, no
  renderer; the task is removed once the activity is idle, as before);
  `$GESHER_APP_WINDOW` gives the default. The launcher tells the system
  server (the `gesher` service's `WINDOW_MODE`), which starts the package's
  processes with `$GESHER_APP_WINDOW` set (and `$GESHER_DISPLAY`, the display
  it describes). With windows the launcher keeps the task and waits for the
  process to end.
- **The hardware renderer is on whenever windows are**: the earlier
  `GESHER_APP_HWUI` switch is gone; only headless processes turn the renderer
  off (with no surface the render thread faulted in CanvasContext).
- **The display** is 400x800 at 160 dpi by default (DeviceProfile, for every
  mode): a phone's 400x800 dp, a pixel a dp, the window's client area.

## Consequences

- No frame crosses a process boundary; the system server knows the app's
  activities but not its windows (window focus, z-order, multi-window and
  per-window visibility from the server are not modelled). A server-side
  window list can be added by having the in-app session report to
  WindowManagerService without changing the drawing path.
- Every window is a top-level Win32 window: dialogs and popups too (not yet
  owned by their parent window, not positioned relative to it).
- Input (the input track): the hook is `WindowSession.attachInput`, called
  when the Win32 window is created; it attaches the window's end of the input
  channel pair to the HWND (`com.gesher.app.input.WindowInput.attach`), which
  then reads the window's event queue (close, resize and focus through its
  listener).
- A process started by the system server for something else (a service, a
  broadcast) gets the device's default window mode; an Activity it shows gets
  a window.
