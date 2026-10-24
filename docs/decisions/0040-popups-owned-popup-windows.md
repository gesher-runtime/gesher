# 0040. Popups: dialogs, menus and toasts are owned popup windows

Status: accepted (popups track, Phase 3+). The coordinator may renumber.

## Context

ADR 0029 made each Android window a top-level Win32 window drawn by the app's
own process. A dialog, a `PopupWindow` (menus, a Spinner's or an
AutoCompleteTextView's dropdown), a tooltip, a toast and an Activity with a
dialog or translucent theme then opened as windows of their own: a taskbar
button each, placed by Windows (`CW_USEDEFAULT`), not moving with the app,
with an opaque black frame where Android leaves the window transparent (a
Material dialog's surface is its card plus room for its shadow), no dim, and
touches outside a modal dialog reached the Activity behind it.

On a phone the window manager lays every window out in the display (the
task's bounds in desktop windowing) by its `LayoutParams` (gravity, x, y, the
attached window's frame), stacks it by its type's layer, composes it with
alpha over what is behind, dims behind a dialog, and InputDispatcher sends a
touch to the topmost touchable window under it, or to a modal window (one
without `FLAG_NOT_TOUCH_MODAL`) wherever it lands, and keys to the focused
window.

Two ways to do that on Windows:

1. **Compose into the app's HWND**: popups draw into buffers the app window
   composes (with alpha, dim and z-order its own), and its input is hit-tested
   and sent to their channels. Exact looks, but popups are clipped to the app
   window, and the composition and input routing would sit in the app
   window's paint and input paths (also used by SkiaGL's swap chain).
2. **Owned popup windows**: each popup is a `WS_POPUP` Win32 window owned by
   the window it belongs to. Windows then keeps it above its owner, hides and
   minimizes it with it, gives it no taskbar button, and the popup keeps its
   own surface, renderer and input channel, as on Android.

## Decision

Owned popup windows (2), with what Windows lacks added by Gesher:

- **Which window a popup belongs to** (`WindowSession.findParent`): a
  sub-window's (`FIRST_SUB_WINDOW..LAST_SUB_WINDOW`: PopupWindow, panels,
  tooltips) is the window its token names (the IWindow it is attached to); an
  application window's (a dialog, `TYPE_APPLICATION`) is its Activity's window
  (same token); a system window's (`TYPE_TOAST`, overlays) is the app's active
  window; an Activity's window whose theme is translucent or floating
  (`ActivityInfo.isTranslucentOrFloating`, what `ActivityRecord.occludesParent`
  asks) is a popup of the window of the Activity below it in its task.
- **Layout and placement**: a popup lays out in its app window's bounds (the
  display, or the size the user resized it to), sub-windows in their parent's
  frame (`ClientWindowFrames.attachedFrame`); its Win32 window is its surface:
  the frame grown by `LayoutParams.surfaceInsets` (ViewRootImpl leaves room for
  the shadow of a window with elevation and the renderer draws that far in),
  put at that place in the app window's client area
  (`Window::SetPopupBounds`); input is offset back to the frame
  (`WindowInput.Options.offsetX/Y`). When the app window moves its popups move
  with it (`WM_WINDOWPOSCHANGED`); when the user resizes it they lay out again
  (`IWindow.resized`). Popups are kept on the screen as Android keeps them in
  the display (`WindowLayout.computeFrames` fits them in the app window).
- **Translucency**: a popup whose format has alpha is a layered window updated
  with `UpdateLayeredWindow` from the CPU pipeline's premultiplied buffer, so
  a dialog's rounded card and shadow show the app behind it. With SkiaGL
  (ANGLE's swap chain presents) popups stay opaque.
- **Dim**: the app window dims what it shows by the topmost `FLAG_DIM_BEHIND`
  popup's `dimAmount` (black at that alpha over its pixels, in its paint).
- **Z-order**: popups of an app window are raised in the order of their
  Android layer (application windows 2, toasts 8, other system windows 10,
  overlays 13; sub-windows above their parent by sub-layer; then the order
  they were added).
- **Input routes** (`Window::SetInputRoute`): an app window and its popups form
  a group; the window events of a group member can go to another member's
  queue, translated through the screen. A modal popup (focusable, touchable,
  without `FLAG_NOT_TOUCH_MODAL`) gets the pointer events of the windows below
  it (a click outside a dialog cancels it, as `Dialog.onTouchEvent` and
  `PopupWindow`'s decor do on Android, and never reaches the Activity); a
  gesture stays with the window its first press went to; the topmost
  focusable window gets the keys and characters; popups with
  `FLAG_WATCH_OUTSIDE_TOUCH` get a press and release where a gesture starts
  outside them (in place of `ACTION_OUTSIDE`). A popup that is not focusable
  is `WS_EX_NOACTIVATE` and starts unfocused; one that is not touchable is
  also `WS_EX_TRANSPARENT` (clicked through). With hidden windows (CI, never
  activated) the session gives the Android focus to the topmost focusable
  window itself; with shown windows Windows' activation does, and activating
  an app window under a modal popup brings the popup to the front.
- **Closing** a popup (Alt+F4, `WM_CLOSE`) is Back (`KEYCODE_BACK`) in it; a
  popup that is an Activity's window (a dialog-themed Activity) finishes it.
- **Toasts** are hosted in the app's process (`ToastHost`): on a device
  `Toast.show()` goes to NotificationManagerService and SystemUI draws text
  toasts; Gesher has no SystemUI and draws each process's windows in it, so
  the process's `Toast.sService` is a proxy that queues toasts one at a time,
  shows a text toast as SystemUI's ToastUI does (ToastPresenter's view in a
  `TYPE_TOAST` window, default gravity and offset), lets a custom toast's TN
  show itself, hides each after 2 s or 3.5 s, and passes every other call to
  the system's `notification` service.
- **Activities in a task** share one place: an Activity's window opening on
  top of another of its task takes the task window's size (its bounds, if the
  user resized it) and position; the window below is hidden when its Activity
  stops; when the top Activity finishes the one below is shown again where the
  task's window was. So a task looks like one window whose content changes, as
  on a phone (and Android's desktop windowing), while each Activity keeps its
  own Win32 window, surface and input channel. The window's title is the
  Activity's TaskDescription label when it set one, else its label; its icon
  the package's (ADR 0029). Under a translucent or floating Activity the
  window below stays shown with its last frame even if the system server stops
  its Activity.

## Consequences

- Popups can extend past the app window only as far as Android lets them
  extend past the display (`FLAG_LAYOUT_NO_LIMITS`); there is no clipping of a
  popup to the app window beyond that.
- Media sub-windows (`TYPE_APPLICATION_MEDIA`, below their parent on Android)
  stack above it here (Windows keeps owned windows above their owner).
- The system server stops the Activity below a translucent or floating one
  (it does not know the theme); the session keeps that window shown, frozen.
  A server-side `occludesParent` would keep it resumed-but-paused as Android
  does (services track).
- Win32 activation and the Android focus can disagree for a moment when the
  user clicks an app window behind a modal dialog (the dialog is brought back
  to the front).
- Windows created by different threads with an owner relationship share
  their input state (`AttachThreadInput` implicitly); each Gesher window's
  thread only runs its message loop, so neither can block the other.
