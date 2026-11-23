# Control Center metadata work follows view visibility

Refs #178, START-06. This change targets the Control Center's general metadata
refresh. It does not change Android application inputs, permissions, package
operations, device lifecycle, update transport or cancellation semantics.

## Previous behavior

At base `8b316aebf09ab46d5407e955bf86ed748a1e739f`, a shared 500ms WPF timer
checked dashboard file stamps every tick and checked device PID, shortcut and
icon stamps every fourth tick. These scans continued while the window was
hidden or minimized. The same timer also completed asynchronous actions,
handled activation requests and advanced update progress. Stopping the whole
timer would therefore break unrelated work.

## Scheduling and notification ownership

`Invoke-PanelHeartbeat` continues to service action completion, activation,
update progress and cancellation. The independent metadata duty returns before
any file/PID/app-list scan while WPF reports the window hidden or minimized.
Completing a relevant user action still performs its existing final refresh;
this is not a recurring background scan.

While visible, metadata changes normally arrive through a managed observer.
`FileSystemWatcher` and process-exit callbacks only mark a generation and queue
one managed notification. They never invoke PowerShell directly. A background-
priority `DispatcherSynchronizationContext` delivers the notification on the
WPF owner thread; an explicit thread check rejects an incorrect context.
The PowerShell handler only marks the view dirty. A 400ms debounce combines
bursts, and a two-second maximum delay prevents continuous changes from
postponing visible refresh forever. A generation change during a read retains
a follow-up refresh instead of acknowledging data that was never displayed.

Visible views compare metadata stamps and PID every five seconds to recover
lost notifications, unwatched external paths, watcher errors and stale ready
files. Hidden views do not run that recovery. Show/restore/activation performs
a fresh read even if no event was delivered. Directory/file replacement and
external install/uninstall therefore remain recoverable. A failed read retains
pending work, and action-completion failure still releases the busy guard.

The recovery inputs cover every metadata file consumed by this view:

| Consumed input | Notification and recovery input |
| --- | --- |
| Ready PID file, packages list, microG marker/APK | Explicit watches and state stamp; the PID also has exit observation and visible liveness recovery |
| Per-app separate-window preferences | Explicit watch and state stamp, including creation and deletion |
| Default icon | App-file observation and recovery even while absent |
| Current apps' shortcut properties and conventional icon | App-file observation and recovery |
| Declared external or custom icon candidates | App-file observation and recovery even while missing, regardless of the currently selected fallback |
| Selected existing icon | App-file observation and recovery, deduplicated with declared paths |

Icon declarations are retained separately from the selected renderable path.
An unavailable declaration does not displace an existing fallback or count as
a decoded image. Its later creation, deletion or replacement is discoverable
without rewriting the shortcut. Declared paths consume the same eight-watcher
budget; over-budget paths remain in the visible recovery stamp.

## Resource ownership

- At most eight filesystem watchers, each with an 8KiB notification buffer.
  One covers the runtime metadata subtree; remaining slots cover external icon
  directories. Additional paths remain covered by visible stamp recovery.
- At most one retained process observer for the PID in the existing ready file.
  Ordinary query `Process` objects are disposed, replacement observers retire
  the previous handle, and actual process exit marks the view dirty.
- One queued dispatcher notification, regardless of event burst size.
- Window close disposes watcher/process resources and suppresses queued
  callbacks. Final cleanup also disposes the observer if dialog exit fails.
- The existing icon budget remains the current view's unique icon set, with
  frozen image reuse across unchanged refreshes and no retained historical
  cache. Removed applications release their cache entries; an empty view has
  none. Existing icon reuse and virtualization contracts continue to run.

The observer shares the panel's existing taskbar-helper `Add-Type` invocation.
There is no second startup compiler invocation or compiler use in callbacks.
The compiled source is larger; this is not evidence of unchanged cold-start
latency. The Windows lifecycle gate reports that compilation duration for
inspection, without treating it as an end-to-end startup benchmark.

## Evidence and limits

`ControlPanelRefreshChecks.ps1` executes production scheduling/refresh methods
with declared data/render boundaries. It covers debounce, busy actions,
transient-read retry, releasing the busy guard after failure, changes during
reads, PID-only recovery and maximum notification delay.

`ControlPanelRecoveryChecks.ps1` deliberately supplies no notifications and
executes the actual package/preference parsing, app projection, signatures,
stamps and scheduler. It checks state-file creation/deletion, displayed
separate-window preference changes, late external/custom/default icons,
fallback retention, candidate deletion/replacement and removal after uninstall.
Only bitmap decoding is a declared boundary; the existing Windows icon-cache
gate additionally decodes a real late-created external image through WPF.

`ControlPanelIdleChecks.ps1` runs the production monitor, event wiring,
filesystem stamps, process observer and heartbeat through a real WPF window
and dispatcher on Windows PowerShell5.1. Real temporary files and an owned
process exercise hidden/minimized scan suppression, continuing heartbeat
duties, restore/activation after external install/uninstall, shortcut bursts,
UI-thread callback ownership, device exit, the watcher budget, missed-event
recovery, actual window-close disposal and suppression of pending callbacks.
Application-list parsing/rendering and other heartbeat duty bodies are declared
fixture boundaries. Their existing action/request/update/icon/virtualization
gates remain enabled. The lifecycle gate runs on Windows2022 and Windows2025,
and in installer validation.

Local Linux verification covers PowerShell parsing, compilation of the managed
observer, production scheduling contracts and a real filesystem/process smoke
test with a queued owner context. It does not execute WPF or establish Windows
behavior. Actual Windows CI is required. No measured CPU, memory or cold-start
improvement is claimed by this change; a full performance comparison still
needs the fixed-workload measurement process from the performance epic.

The branch includes the exact independently reviewed CI foundation from PR226
(`42417047c95321c4e2257b05bdf5900c11c10a74`) and targets `main`, preserving
the normal ARM64 and CodeQL workflow triggers.
