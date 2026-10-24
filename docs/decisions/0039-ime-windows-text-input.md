# 0039: Windows' text input as the input method, in the windows' process

Status: accepted (ime track)

## Context

Phase 3 gave app windows their input through an InputChannel (ADR 0027):
keys arrive as KeyEvents translated from virtual-key codes through
Generic.kcm, a US layout, and characters the key map cannot type (WM_CHAR)
reached the focused view as `ACTION_MULTIPLE` KeyEvents. With a Hebrew
keyboard layout a key typed both its US letter and its Hebrew one; IMEs
(Japanese, Chinese, Korean) could not compose; `input_method` was a stand-in
with no input method, so `InputMethodManager.isAcceptingText`,
`CursorAnchorInfo`, `performEditorAction` and the rest of the input method
contract did not happen. On Android an input method is a separate app bound
by InputMethodManagerService; on Windows an IME (IMM32 or a TSF text
service) runs inside the process whose window has the focus, and an app
sees its text as WM_CHAR and WM_IME_* messages.

## Decision

1. **The input method runs where the windows are.** As the window session
   runs in the drawing process (ADR 0029), Windows' text input is the input
   method of each process with Win32 windows:
   `com.gesher.app.input.WindowsInputMethod` is the host of an
   `InputMethodManagerService` (`com.gesher.server`, registered by
   `Services` as `input_method`). In a single process that is the service
   Services made; in an app process of the Gesher device the process makes
   its own instance and points `IInputMethodManagerGlobalInvoker`'s cache
   at it (the system server's instance has no windows; it answers the input
   method list and subtypes for everyone else). One input method, "Windows
   text input"; its subtypes are the session's keyboard layouts
   (GetKeyboardLayoutList), the current one the focused window's thread's.
2. **A real binding.** `startInputOrWindowGainedFocus` gives the client an
   `InputBindResult` with the window's `IInputMethodSession` (and an
   InputChannel, so key events go through the input method stage as on a
   device; it hands every one back), makes the client active and reports
   the editor's `EditorInfo` and `IRemoteInputConnection` to the host. So
   `isActive`, `isAcceptingText`, `restartInput`,
   `onInputConnectionOpened`, `updateSelection` and `updateCursorAnchorInfo`
   behave as with an IME. The host talks to the editor as an IME does:
   `commitText`, `setComposingText`, `finishComposingText`,
   `performEditorAction`, `setSelection`, `requestCursorUpdates` on the
   `IRemoteInputConnection`, with the session id `invalidateInput` gives.
3. **The Win32 side subclasses the window procedure** (compat/input/ime.cc,
   attached with each input connection; gesher::Window's code is
   unchanged). At WM_KEYDOWN, TranslateMessage has already queued what the
   key types in the thread's layout, so it is peeked: while an editor has
   the focus, a key whose KeyEvent (Generic.kcm, with the thread's
   modifiers) would type something else — a Hebrew or AZERTY key, AltGr, a
   dead key — is the input method's, and its WM_CHAR becomes text; a key
   whose KeyEvent types its own character keeps the KeyEvent and its
   WM_CHAR is dropped. Without an editor nothing is taken (games keep their
   key codes) and other characters still reach the focused view as
   `ACTION_MULTIPLE`. VK_PACKET (SendInput's Unicode input: the emoji panel,
   dictation, on-screen keyboards) and VK_PROCESSKEY (a key an IME took) are
   always the input method's. Editing keys, arrows, Backspace, Delete and
   the Ctrl shortcuts stay KeyEvents, which TextView handles as with a
   hardware keyboard on Android.
4. **Text in order with keys.** Each text event is handed to the window's
   own procedure as a WM_CHAR of a marker value, so gesher::Window queues it
   among the keys; the connection hands the event to Java when it reaches
   the marker, after the events before it are finished, and holds what
   follows until the host has applied it (a query on the InputConnection
   after it answers), or a second.
5. **IMM32, inline.** WM_IME_SETCONTEXT drops ISC_SHOWUICOMPOSITIONWINDOW;
   WM_IME_STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION are not passed on:
   GCS_RESULTSTR is committed, GCS_COMPSTR is the composing text (its
   GCS_CURSORPOS caret, its GCS_COMPATTR target clause highlighted), an empty
   composition cancels. IMM32 rather than a TSF text store: Windows runs TSF
   text services (Microsoft IME for Japanese, Pinyin, Korean) for IMM32
   windows through its compatibility layer, the messages are per window and
   the code stays small. Without an editor (or in a password, number or
   ASCII-only field) the window has no input context
   (`ImmAssociateContextEx`), so IMEs stay out.
6. **The caret.** The editor's CursorAnchorInfo (CURSOR_UPDATE_MONITOR)
   becomes the system caret (CreateCaret, never shown; screen readers,
   magnifiers and IMEs read it), the composition window's position
   (CFS_POINT), the candidate window's (CFS_EXCLUDE around the caret) and
   the answer to IMR_QUERYCHARPOSITION.
7. **Enter performs the editor's action** (`performEditorAction`) when the
   EditorInfo has one (single line, not IME_FLAG_NO_ENTER_ACTION), as an
   IME's Enter key does.
8. **The soft input is Windows' touch keyboard**, shown (InputPane.TryShow
   through IInputPaneInterop) when the app asks for the soft input
   (`showSoftInput`, which reaches the window session as requested visible
   types) and the editor was focused by a finger or a pen
   (`EditorInfo.getInitialToolType`), as Windows does for its own fields;
   hidden with `hideSoftInputFromWindow`.

## Consequences

- Typing in any keyboard layout gives that layout's text; IME compositions
  show inline in the editor, with the candidate window at the caret.
- Apps that watch KeyEvents for letters see none for the keys the input
  method takes while an editor has the focus (as with a soft keyboard on
  Android); without an editor they get the key codes.
- The C API (gesher_input.h) keeps characters as kChar window events; it
  gets no compositions.
- TSF-only features (reconversion, a TSF text store's document access) are
  not offered; the touch keyboard does not resize the window (no IME
  insets).
