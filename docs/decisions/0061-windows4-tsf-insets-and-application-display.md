# 0061. Text services, IME occlusion and the shared Application display

Status: proposed (windows4). Native fixture scope verified in standalone CI
40414421254 at a827d5c; current Android/JNI and full-suite verification is pending.

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

DisplayManagerGlobal's private cache is disabled in every app client before
Application code. Its Android 16 invalidation path requires a native shared
nonce store that Gesher lacks; the first CI exposed that as a Signal Generator
crash on both runners. Fresh Binder snapshots preserve correct Display and
listener behavior without invoking unsupported cache invalidation. Shared
nonce storage is not claimed by this choice.

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

The COM-source IFrameworkInputPane test passes standalone CI 40414421254 on
Windows 2022 and 2025. That fixture uses the real production HWND, COM source
and condition-variable queue; real gesher-base/eventfd and Android delivery
remain a separate full-CI requirement.
It exercises the same subscription/handler as real windows, including an
actual window's queued events, full rectangle geometry and reference/cookie
lifetime. Apps keep the OS COM source; only a native probe supplies a factory.
An input pane horizontally outside a window no longer obscures that window's
rows. Physical keyboard/IME behavior is not inferred from this source fixture.
See Microsoft's [IFrameworkInputPane](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iframeworkinputpane)
and [Showing](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iframeworkinputpanehandler-showing)
contracts for the interface and screen rectangle.

TSF text-store source is work in progress, now connected to InputConnection
through complete ExtractedText snapshots and ordered range edits over JNI.
It implements ITextStoreACP locking/UTF-16 access, selection and edit events,
composition-sink events and revision/epoch protection against stale replies.
A restricted/unavailable document is cleared and its thread manager detached.
Native COM sink/service tests, actual thread-manager activation and a real
service DLL pass standalone CI 40414421254, including the production native
IME adapter, reentrant owner teardown, epochs, range geometry and partial
compositions. A real EditText fixture exercises both process modes in the
next combined CI. TSF key
filtering runs before TranslateMessage queues character messages; existing
IMM32, password/number, caret and action paths still need full regression CI.
Caret geometry and actual CursorAnchorInfo composing-character bounds are
defined. Measured ranges are accepted only for matching editor epoch/text,
with screen mapping, clipping and hit testing; missing/stale measurements
return TS_E_NOLAYOUT. Rich formatting/embedded objects are unsupported.
See [RequestLock](https://learn.microsoft.com/en-us/windows/win32/api/textstor/nf-textstor-itextstoreacp-requestlock)
for notification/reentrancy requirements. Current Java/JNI composing spans
and real Microsoft candidate positioning are not established by native-only CI.
Candidate-window discovery remains unimplemented.
The actual Microsoft candidate HWND already observed by the SendInput IME
fixture is now checked while visible: a floating candidate must leave Android
IME edge insets empty and content unresized. This follows Android's
[floating IME contract](https://developer.android.com/reference/android/view/WindowInsets.Type#ime()).
That is a real candidate UI observation, not a claim that the runtime discovers
arbitrary candidate windows or distinguishes candidate-only content/visible
insets for every third-party IME. Its CI result is pending.
A target-SDK-28 APK and show/hide resize fixture are added for old-target
adjustResize, without manual app padding. Its result is not yet verified;
no production resize change is justified before that observation. The Android
resize fixture uses the existing keyboard-message seam while the native COM
probe separately exercises the actual callback/source/window-event boundary.
Physical touch keyboard behavior requires hardware and remains unverified.

The source geometry preserves a visible floating pane separately from edge
occlusion: a rectangle must span the client's complete bottom edge to become
bottom rows; a floating overlap has an empty visible IME source frame. The
real COM probe covers middle/narrow floating rectangles and redocking, while
the old-target APK checks visible-with-zero-insets content behavior. Native
local proof does not replace the pending Android/runtime CI verification.
