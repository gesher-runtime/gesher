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

TSF document/reconversion, COM-source IFrameworkInputPane callback tests,
candidate-window occlusion and old-target adjustResize are not implemented.
Physical touch keyboard behavior requires hardware and remains unverified.
