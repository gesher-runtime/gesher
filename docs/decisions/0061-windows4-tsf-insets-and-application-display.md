# 0061. Text services, IME occlusion and the shared Application display

Status: proposed (windows4). No CI verification yet.

## Context

ADR 0053 gives each task its own rotation, while the shared display service
always returned natural orientation. IME tests posted an internal message;
actual IFrameworkInputPane callbacks, candidate occlusion, old-target resize
and TSF document access/reconversion remain open in this track's brief.

## Work in progress

The shared default display returned to an Application's DisplayManager follows
the focused visible task. Each Activity retains an explicit task rotation,
including zero, so an unfocused portrait task does not inherit another task's
landscape orientation. DisplayInfo uses the rotated logical dimensions while
the physical display mode remains natural. Real display callbacks carry basic
display changes and respect each client's event mask. A user resize changes
the task's bounds, not the shared display's natural dimensions.

This is a single shared display: an Application context describes the focused
task, rather than identifying the Activity that happened to fetch the service.
Activity contexts remain the way to query a particular task. Single-process
orientation changes update the same service in the app process; device mode
updates it in the system server with the top-resumed task.

## Open verification

54-windows4.ps1 has executable cases for existing Display objects and registered
listeners, multiple visible portrait/landscape tasks, focus changes and both
process modes; their runtime result is pending. The new display helper and TaskConfiguration compile against saved
framework/core classes locally; that is syntax evidence only. No runtime
behavior or roadmap milestone is claimed.

The COM-source IFrameworkInputPane test is implemented but not CI verified.
It exercises the same subscription/handler as real windows, including an
actual window's queued events, full rectangle geometry and reference/cookie
lifetime. Apps keep the OS COM source; only a native probe supplies a factory.
An input pane horizontally outside a window no longer obscures that window's
rows. Physical keyboard/IME behavior is not inferred from this source fixture.
See Microsoft's [IFrameworkInputPane](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iframeworkinputpane)
and [Showing](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iframeworkinputpanehandler-showing)
contracts for the interface and screen rectangle.

TSF document/reconversion and candidate-window discovery are not implemented.
A target-SDK-28 APK and show/hide resize fixture are added for old-target
adjustResize, without manual app padding. Its result is not yet verified;
no production resize change is justified before that observation. The Android
resize fixture uses the existing keyboard-message seam while the native COM
probe separately exercises the actual callback/source/window-event boundary.
Physical touch keyboard behavior requires hardware and remains unverified.
