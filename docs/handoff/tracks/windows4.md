# Track brief: windows4

Window/input leftovers after windows3 (merged at 80cb28b, code verified at
a15ade4, ci 40285476400). ADR 0061 is reserved for this track. Worktree:
`C:\work\gesher-windows4`, branch `wip/windows4` from phase4/integration.

Read track-rules.md, docs/windows3.md, docs/input.md, docs/locale.md, ADRs
0039, 0049 and 0053, compat/input/ime.cc and the window/session code first.
Use absolute paths in this worktree. CI only for AOSP; no WSL.

Goals, each with CI evidence on both Windows runners:
- TSF reconversion and document access: connect the focused InputConnection
  to the Windows text service so existing text can be selected and reconverted.
  Preserve the IMM32 composition path, password/number restrictions, caret
  positioning and editor actions. Cover document edits and reconversion through
  an actual TSF interface, with a test text service if the runner's IME cannot
  automate it; identify any real-IME behavior not exercised.
- IME candidate-window occlusion as Android IME insets where Android reports
  it. Test actual IFrameworkInputPane handler callbacks through a COM test
  source, rather than only posting kMsgInputPane. Keep physical touch keyboard
  verification explicitly pending if CI has no supported keyboard.
- Legacy adjustResize behavior for older target SDKs, alongside the verified
  edge-to-edge inset and adjustPan paths.
- Reconcile a task's rotation with Display information available from its
  Application context; cover multiple visible tasks with different rotations
  and document which display the shared Application context describes.

Tests: tests/windows/tracks/54-windows4.ps1 and focused additions to winapp/ime
test apps (one build/copy line per app in framework.yml). Domain: compat/input,
compat/hwui/window, app window/input glue and their docs; keep shared edits
minimal. Merge current origin/phase4/integration before final CI. Report every
PASS line, verified SHA, package/e2e conclusions and anything still unverified.
No roadmap checkmark for an implementation without CI verification.

## Status and next steps

Resumed on 2026-10-26 after merging current integration a57c1f8 without a reset.
WIP: the shared Application Display follows the focused visible task, with
explicit per-Activity zero/rotated overrides and masked Binder display callbacks.
ApplicationDisplay/TaskConfiguration compile against saved framework/core
classes locally; the complete app/framework build and runtime tests await CI.
54-windows4.ps1 now drives real DisplayActivity windows with held/fresh
Application Display objects and listeners, orientation changes, two visible
portrait/landscape tasks and foreground switches. The test helper's PowerShell
syntax passes; runtime results are pending. The latest verified AOT3 integration
f5ce6f6 is merged. No tests or roadmap milestones are complete. Next: inspect
the first display-stage CI, then TSF document/reconversion, real COM-source
input-pane callbacks/candidate occlusion and old-target adjustResize. ADR 0061
records the work. The physical touch keyboard remains unverified.

First full display-stage CI: **40377891527** at
`dd6e65fe64196542ca8ae99d8953b8fdd39a7f89`; watch log
`C:/work/gesher-ci-review/windows4-40377891527-watch.log`. Complete build,
both Windows suites, actual package cleanup and both e2e results are pending.

The first run's asm-port job failed before its checks on upstream promisor
fetch HTTP 502/expected-packfile errors (job 121765852176, exit 123). Other jobs
continue for diagnostics; this run cannot establish a fully green milestone.

Separate WIP after that dispatch: extracted the real IFrameworkInputPane
subscription into input_pane.cc, retaining the complete callback rectangle.
Native COM-source probe exercises QueryInterface, AdviseWithHWND, Showing,
Hiding, Location, opaque zero cookies, failed Advise and destruction through
a real Gesher window's event queue. Its optional source factory is used only
by the native probe; apps continue activating Windows' source. Module and probe
compile with local g++ -Wall -Wextra -Werror (syntax only). CMake/graphics.yml
build and collect the probe; actual CI runtime verification is still pending.
Candidate-window discovery, TSF and old-target resize remain open.

The next source also builds a separate target-SDK-28 winlegacy.apk and runs
its unpadded content through IME show/hide in both process modes, checking
framework content height, editor bounds and unchanged native client size.
It uses the existing keyboard-message injection for the Android resize path;
the separate COM probe verifies the real source/handler/window-event path.
The legacy fixture compiles locally against saved framework classes and the
PowerShell parses, but no runtime behavior is claimed yet. No production
resize workaround has been added without a failing runtime observation.
