# Track brief: lifecycle

Window and activity lifecycle leftovers from the popups track. The original brief given to the first agent of this track (2026-10-25), kept verbatim. A successor reads it together with the "Status and next steps" section in the track's doc on its branch.

You are the "lifecycle" track agent of the gesher project. Your worktree is C:\work\gesher-lifecycle on branch wip/lifecycle (cut from phase4/integration and pushed). Work only there.

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding. Then read docs/services.md, docs/graphics.md (app windows), the popups docs and ADR 0040 (popups as owned popup windows), ADRs 0019, 0024-0026, 0029, 0030, 0046, and com.gesher.server's activity/task code.

GOAL: the activity lifecycle and window behavior Android apps observe, where Gesher differs today. The popups track reported:
- The system server stops the Activity under a translucent or dialog-themed Activity; Android only pauses it (it stays visible). Implement Android's visibility rules: an activity behind a translucent/floating one (windowIsTranslucent, windowIsFloating, Theme.Dialog, Theme.Translucent) is paused, not stopped, and its window stays shown under the new one; it is stopped only when an opaque, fullscreen activity covers it.
- Popups are opaque under SkiaGL: translucent popup/dialog windows (rounded corners, shadows, dim behind) must composite with alpha over what is under them (layered windows / DirectComposition / DWM; decide and record it in an ADR docs/decisions/0048-lifecycle-translucency.md). FLAG_DIM_BEHIND dims the parent.
- No real ACTION_OUTSIDE: a window with FLAG_WATCH_OUTSIDE_TOUCH gets ACTION_OUTSIDE for a click outside it; a PopupWindow with outsideTouchable dismisses as on Android.
- Minimizing the window doesn't stop the Activity: minimize → onPause/onStop (onSaveInstanceState as Android orders it), restore → onRestart/onStart/onResume; losing focus to another Windows app → onWindowFocusChanged(false) but stays resumed (as in Android multi-window); closing the window is "back" (finish) as today.
- Anything else you find in the same area that real apps hit (configuration changes on window resize: onConfigurationChanged vs recreate per configChanges, screen size/orientation qualifiers from the window size).
- Tests in tests/windows/tracks/48-lifecycle.ps1 (a test APK recording lifecycle callbacks; pixel read-backs for translucency and dim), both runners, single process and on the Gesher device.
Your file domain: com.gesher.server's activity/task/visibility code, the Win32 window host code for app and popup windows (shared with the ime and webview tracks: minimal, careful edits), tests/windows/tracks/48-lifecycle.ps1 and its test app (framework.yml: one line per test app), docs (a lifecycle section in docs/services.md or a new docs/lifecycle.md), ADR 0048, the roadmap (add boxes under Phase 4 or Phase 8 as fits).

Coordinator notes: other tracks run in parallel on the shared CI: trigger CI only when code is ready, cancel superseded runs. Watch with `gh run watch <id> -R gesher-runtime/gesher --exit-status --interval 120`; on HTTP 403 back off ~10 minutes. Keep a "Status and next steps" section updated with every meaningful push. Before your final CI run, merge origin/phase4/integration into your branch. Final message = the report track-rules.md asks for (run ids, PASS lines, remaining gaps, edits outside your domain, the verified SHA).
