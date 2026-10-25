# Track brief: windows3

Window and activity leftovers reported by the lifecycle, webview2, ime and
media tracks. The original brief given to the first agent of this track
(2026-10-25), kept verbatim. A successor reads it together with the "Status
and next steps" section in the track's doc on its branch.

You are the "windows3" track agent of the gesher project. Your worktree is
C:\work\gesher-lifecycle (reused) on branch wip/windows3 (cut from
phase4/integration and pushed). Work only there.

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/lifecycle.md, ADR 0048, docs/graphics.md (app windows),
docs/input.md (the ime section), docs/webview.md (the webview2 limits),
docs/services.md and com.gesher.server's activity/task code.

GOAL: close these known gaps, each verified in CI:
- Screen orientation: Gesher has none. An activity's `screenOrientation`
  (landscape, portrait, sensor*, user*, locked) and `setRequestedOrientation`
  must give it a window and a configuration of that orientation
  (Configuration.orientation, Display rotation and size, resource
  qualifiers -land/-port), as Android does on a device that rotates. Today
  Signal Generator only passes e2e because e2e gives it an 800x400 display:
  after this, it must pass on the default display (drop the override in
  tests/e2e/apps.json).
- The app window's thread does not wake for input when its input queue is
  attached to WebView2's (WebView2 runtime 131, windows-2022: a click on an
  app view inside a WebView cut-out waits until something posts to the
  thread; 37-webview's Features.ps1 posts WM_NULL after 20 s as a
  workaround). Fix the window thread in compat/hwui/window (MsgWaitForMultipleObjectsEx
  flags / QS_ALLINPUT / MWMO_INPUTAVAILABLE) and remove the workaround.
- Only one activity is resumed at a time: implement Android 10+'s
  multi-resume for activities in separate visible windows (each visible top
  activity resumed; onTopResumedActivityChanged for the focused one).
- An app's own `Activity.moveTaskToBack` hides its windows: it must minimize
  them, and restoring from the taskbar brings the task back.
- The single process does not relaunch an activity for configuration changes
  it doesn't declare: make it relaunch as the device path does.
- IME insets: the Windows touch keyboard (and the IME candidate area where
  Android would report it) gives no `WindowInsets.Type.ime()`; report the
  touch keyboard's occluded rect (IFrameworkInputPane) as IME insets and
  `adjustResize`/`adjustPan` behave as on Android.
- Clipboard: check whether the `clipboard` service (devsvc track) syncs with
  Windows' clipboard in both directions; if not, make ClipboardManager and
  Ctrl+C/V/X in EditText use Windows' clipboard (text, HTML, URIs as text),
  with clipboard change listeners fed by AddClipboardFormatListener.

Tests in tests/windows/tracks/51-windows3.ps1 (test APKs of your own; pixel
and window-rect read-backs, SendInput), both runners, single process and on
the Gesher device. Your file domain: com.gesher.server's activity/task code,
the Win32 window host code (compat/hwui/window, shared: careful edits),
WindowSession/WindowMode, the clipboard service, tests/windows/tracks/51-windows3.ps1
and its test apps (framework.yml: one line per test app), docs
(docs/lifecycle.md sections or a new docs/windows3.md), ADR
docs/decisions/0053-windows3-orientation-multiresume.md (0053 is reserved for
you), the roadmap boxes.

Coordinator notes: other tracks run in parallel on the shared CI (media,
arm64, hwsvc2, aot2, exec2): trigger CI only when code is ready, cancel
superseded runs. Watch with `gh run watch <id> -R gesher-runtime/gesher
--exit-status --interval 120`; on HTTP 403 back off ~10 minutes. Keep a
"Status and next steps" section updated with every meaningful push. Before
your final CI run, merge origin/phase4/integration into your branch. Final
message = the report track-rules.md asks for (run ids, PASS lines, remaining
gaps, edits outside your domain, the verified SHA).
