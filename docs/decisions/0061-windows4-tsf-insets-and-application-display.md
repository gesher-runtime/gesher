# 0061. Text services, IME occlusion and the shared Application display

Status: proposed (windows4). Native fixtures pass standalone40440617227 at
5b5b51d on both runners. Full40447661700 at8d97726 provides actual Android
evidence below but is red: four failed checks per runner. Corrected non-editor
fixture and a complete zero-failure combined run remain required.

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

Top-level task activation is distinct from keyboard focus. The implementation
delivers WM_ACTIVATE as a separate native activation event and retains default
Win32 focus handling; it does not synthesize a keyboard-focus transition.
The native fixture checks activation while the real HWND already has keyboard
focus, with a counterfactual copy that omits only this event. Standalone
40440617227 verifies that negative copy; actual strict foreground HWND
preconditions and both two-task Android transitions pass full40447661700 on
both runners. Overall stage acceptance still requires a complete green run.
See the [WM_ACTIVATE contract](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-activate).

## Open verification

54-windows4.ps1 has executable cases for existing Display objects and registered
listeners, multiple visible portrait/landscape tasks, focus changes and both
process modes. Their held/fresh shared/task Display snapshots, real foreground
preconditions and listener transitions pass full40447661700 in both actual
Windows logs. The actual ART Configuration and CursorAnchorInfo assertions
also pass, including Hebrew/Arabic/English direction at four rotations and
replacement of missing measured geometry. This scoped evidence does not make
the red full suite or a roadmap milestone accepted.

The COM-source IFrameworkInputPane test passes standalone CI40440617227 on
Windows 2022 and 2025. That fixture uses the real production HWND, COM source
and condition-variable queue; the same native probes also pass the full run
with real gesher-base. Android legacy/insets observations are separate cases,
not a claim that hardware COM events were driven end to end through ART.
It exercises the same subscription/handler as real windows, including an
actual window's queued events, full rectangle geometry and reference/cookie
lifetime. Apps keep the OS COM source; only a native probe supplies a factory.
An input pane horizontally outside a window no longer obscures that window's
rows. Physical keyboard/IME behavior is not inferred from this source fixture.
See Microsoft's [IFrameworkInputPane](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-iframeworkinputpane)
and [Showing](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-iframeworkinputpanehandler-showing)
contracts for the interface and screen rectangle.

The TSF text store is connected to InputConnection
through complete ExtractedText snapshots and ordered range edits over JNI.
It implements ITextStoreACP locking/UTF-16 access, selection and edit events,
composition-sink events and revision/epoch protection against stale replies.
A restricted/unavailable document is cleared and its thread manager detached.
Native COM sink/service tests, actual thread-manager activation and a real
service DLL pass standalone CI40440617227, including the production native
IME adapter, reentrant owner teardown, epochs, range geometry and partial
compositions. Full40447661700 proves real EditText reconversion, live Java
composing spans, Japanese completion/caret9, password/number rejection,
reconnection and aXcd/caret2 after a partial composition in both modes on both
runners. Its newly added non-editor observation fails because the first touch
takes Button focus and deliberately skips onClick in pinned View.java; the
prepared correction observes actual focus gain using the same strict oracle.
That correction is not yet runtime verified. TSF key filtering runs before
TranslateMessage queues character messages. The unchanged IME35 tests,
including real Microsoft SendInput conversion and exact native commit, pass
both modes/both runners in that full run.
Caret geometry and actual CursorAnchorInfo composing-character bounds are
defined. Measured ranges are accepted only for matching editor epoch/text,
with screen mapping, clipping and hit testing; missing/stale measurements
return TS_E_NOLAYOUT. Rich formatting/embedded objects are unsupported.
See [RequestLock](https://learn.microsoft.com/en-us/windows/win32/api/textstor/nf-textstor-itextstoreacp-requestlock)
for notification/reentrancy requirements. Java/JNI spans and actual Microsoft
candidate positioning use the full Android evidence, not native-only CI.
Candidate-window discovery remains unimplemented.
The actual Microsoft candidate HWND already observed by the SendInput IME
fixture is now checked while visible: a floating candidate must leave Android
IME edge insets empty and content unresized. This follows Android's
[floating IME contract](https://developer.android.com/reference/android/view/WindowInsets.Type#ime()).
That is a real candidate UI observation, not a claim that the runtime discovers
arbitrary candidate windows or distinguishes candidate-only content/visible
insets for every third-party IME. Both actual full-run logs show the observed
floating candidate leaves Android edge insets empty and native content unchanged.
A target-SDK-28 APK and show/hide resize fixture are added for old-target
adjustResize, without manual app padding. Both full-run logs verify framework
content shrinks from800 to500 and restores, with unchanged400x800 native client.
The existing pinned ViewRootImpl compatibility policy supplies this behavior;
no unconditional server fit-insets override is introduced. The Android
resize fixture uses the existing keyboard-message seam while the native COM
probe separately exercises the actual callback/source/window-event boundary.
Physical touch keyboard behavior requires hardware and remains unverified.

The source geometry preserves a visible floating pane separately from edge
occlusion: a rectangle must span the client's complete bottom edge to become
bottom rows; a floating overlap has an empty visible IME source frame. The
real COM probe covers middle/narrow floating rectangles and redocking, while
the old-target APK checks visible-with-zero-insets content behavior. Standalone
native CI does not replace the full Android observations above or the pending
complete zero-failure run with the corrected non-editor fixture and latest
coordinator-verified runtime prerequisites.
