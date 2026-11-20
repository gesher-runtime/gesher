# 0088: One desktop window per app; its tasks take turns in it

Status: accepted (see docs/lifecycle.md "One window per app")

## Context

Each Android task was a Windows window of its own (Android's desktop windowing;
ADR 0040, windows3 track). Activities in one task already share one place: an
Activity started on top of another opens where the task's window is, and the
window below hides as its Activity stops. Apps that open some screens in a task
of their own therefore opened a second window. Gmail's compose screen is one:
`ComposeActivityGmail` has `taskAffinity="com.google.android.gm.compose"`,
`documentLaunchMode="intoExisting"` and `autoRemoveFromRecents`. Every draft is
a document task, a card of its own in a phone's Recents. On a phone the new task
covers the screen, so the user doesn't notice it. On the desktop a second window
opened elsewhere, and placing it over the first does not help: moving it reveals
the other window under it.

Gesher's task resolution is right (it follows ActivityStarter). Merging the
tasks, or dropping `FLAG_ACTIVITY_NEW_DOCUMENT`/`MULTIPLE_TASK`, would change what
the app observes: `getTaskId`, `isTaskRoot`, `onNewIntent`, Back, `getAppTasks`
and Recents. Only the presentation is wrong for a desktop.

## Decision

1. **The tasks of one app are presented as one window.** The group is a task's
   owner package (the root activity's package, the `AppTaskRecord` owner,
   what `getAppTasks` reports). When a task comes to the front (it is started,
   brought forward, or restored), the system server moves the group's other
   visible tasks to the back, as `moveTaskToBack` does. Their activities pause
   and stop (`onPause`, `onStop`, saved state), and their windows hide because
   their Activities stop, as the window of an Activity covered in its task
   does. Each task stays a real Android task, with its own id, history, rotation
   and size. This is what a phone does: the task in front covers the app's
   others, which stop.
2. **A task whose top activity is translucent or floating hides nothing.** On
   a phone the task under it stays visible, and so does the window here.
3. **The group's window keeps its place.** A task window that opens, or shows
   again, takes the place of the app window shown now, else where the app's
   window was last. It also takes its size unless its display is turned for an
   orientation. Moving or resizing the window moves the app. The size reaches
   the task as the user's resize (`resizeTask`), as before.
4. **Back and closing.** Back on a task's last activity finishes it as before
   (Android's rule: a root not launched from home finishes). When the group's
   front task has no activity left, the system server brings the most recent
   task that the group hid back to the front. A task the user minimized is not
   replaced. Gmail's compose window therefore returns to the inbox. The
   window's X closes the app: every task of the package is removed
   (`AppTask.finishAndRemoveTask`, then the window's own task as before). Each
   Activity gets its usual `onPause`/`onStop`/`onDestroy`.
5. **Switching tasks.** With two or more tasks, the window menu (Alt+Space or a
   right click on the title bar) lists the app's tasks, with the shown task
   checked, plus "Close this task". Choosing a task brings it to the front
   (`moveTaskToFront`, as restoring a window does). Without this, the hidden
   tasks would be unreachable, since a phone's Recents has no counterpart here.
6. **Per app, separate windows stay available.** `gesher-app windows PACKAGE
   separate|shared`, or the Control Center's "Separate windows" checkbox, saves
   `separate.<package>=true` in `<data>/system/app-windows.properties`. The
   system server gives a process the setting when it starts it
   (`$GESHER_SEPARATE_WINDOWS`), and uses the value it gave the package's
   processes for the grouping too, so a change applies from the app's next
   start. With separate windows, every task is its own window as before (ADR
   0040).

## Consequences

- One taskbar button and one window per app by default. Two drafts side by side
  need the separate-windows setting.
- The group is per package, so another app's task (a share target, a picker)
  stays a window of its own.
- A hidden task is stopped: it gets no frames and no input, and its window is
  hidden (no taskbar button or Alt+Tab entry). Only the window menu, or
  starting the app again (the launcher intent reaches the existing task),
  brings it back.
- Placement is per process. A task in another process of the same package is
  grouped and hidden by the system server, but its window opens at Windows'
  default place.
- Labels in the window menu come from the task's window in this process (its
  TaskDescription label, else the Activity's title), else the base activity's
  label.
