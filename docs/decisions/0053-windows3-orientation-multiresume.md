# 0053. Orientation per task, multi-resume, minimized tasks, IME insets

Status: accepted (windows3 track, Phase 4; ci 40285476400 at a15ade4,
both Windows runners, package and e2e). The coordinator may renumber.

## Context

ADR 0048 left gaps: no screen orientation (an Activity asking for landscape got
a portrait window; e2e gave Signal Generator a landscape display), one resumed
Activity at a time, `moveTaskToBack` hiding windows, the single process not
relaunching, no IME insets. A click in a window with WebView2 children could
wait in its thread's queue.

## Decision

1. **A task's display rotates for its top Activity's orientation.** Each task
   is a window, so each has a rotation (the server's `mTaskRotation`), chosen as
   a phone without a sensor chooses it. The task's override is the turned
   display's size with the window configuration's rotation, display rotation and
   max bounds, which `Display` and `WindowManager` report to the Activity. The
   window session takes the window's shape from the Activity's configuration
   (the server is the authority, as WM is for task bounds). Alternative: turning
   the process's DisplayInfo; rejected, a process can have tasks of both
   orientations and the framework already prefers the configuration.
2. **Multi-resume**, as Android 10+: pausing is per task; the top resumed
   Activity is the top of the most recently focused visible task, told with
   `TopResumedActivityChangeItem`.
3. **The app process sees its Activities' controller calls**
   (`ActivityClientHook`, a proxy set with `ActivityClient.setActivityClientController`):
   `moveActivityTaskToBack` minimizes the task's windows; the single process
   answers orientation calls for its own token.
4. **The single process relaunches with the window preserved**, as
   `handleRelaunchActivity` does.
5. **IME insets from IFrameworkInputPane**, per top-level window, as an IME
   `InsetsSource`; the framework's adjustResize/adjustPan do the rest.
6. **Window threads wait with MsgWaitForMultipleObjectsEx(MWMO_INPUTAVAILABLE)**
   and a 50 ms backstop.

## Consequences

- Fixed-orientation apps run on the default display; a rotation relaunches
  Activities that do not handle it.
- Other tasks' Activities stay resumed (apps see onTopResumedActivityChanged).
- A minimized task is restored from its taskbar button.
- Every window thread wakes at least every 50 ms.
