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

TSF foundation WIP: text_store.h/.cc implements the native ITextStoreACP and
composition-sink document contract over UTF-16 snapshots, with read/write
locks, deferred lock upgrades/notifications, selection/edit events and
epoch/revision checks for asynchronous replies. It compiles with local g++
-Wall -Wextra -Werror. It is intentionally not wired into the runtime/CMake
yet; no TSF capability is advertised or claimed. Next: native COM sink/service
fixtures, then thread-manager and InputConnection plumbing. Non-caret text
geometry, rich formatting/embedded objects and physical IME behavior are not
established by this WIP.

Own native TSF fixture now passes locally: 18 ACP/sink checks plus five actual
Windows thread-manager/ITfTextInputProcessor/edit-session reconversion checks.
Windows TSF reads a pre-existing Hebrew selection, starts a composition over
that range, replaces it with Japanese text and ends it through the real ACP
document. No system IME profile is registered. The source/probe compile with
g++ -Wall -Wextra -Werror; CMake/graphics.yml and a 45-second bounded Windows
test wrapper are added for eventual CI. This still does not connect Android's
InputConnection to TSF; runtime activation/Java plumbing and CI remain open.

Additional local native proof: the new window-owned TextService adapter uses
AssociateFocus, retains/restores the previous document, and loads an explicit
test-service DLL until after context/thread-manager teardown. Its real DLL
reconverts the adapter's own document and cleanup passes. Portable snapshot/edit
types are in text_document.h; a disconnect clears retained document data and
owner callbacks before COM teardown. These classes are still not wired into
the production input method. Next: epoch/revision JNI/Java snapshot/edit
plumbing and pre-TranslateMessage TSF keystroke routing, preserving IMM32.

First display-stage e2e results: both runners fail Signal Generator on
`unable to assign nonce handle: system_server/display_info`; the other eight
apps, including required TIC-80/Puzzles, pass. This is the new display
invalidation call reaching an unimplemented ApplicationSharedMemory nonce
store, not an accepted flake. Fixed in the next source: remove invalidation and
disable the private DisplayManagerGlobal cache before Application code in
both single-process and device clients, including headless ones. DisplayInfo
then uses fresh Binder queries and the real callback path. Display/listener
tests remain unchanged. CI proof of the fix is pending.

Next WIP connects the TSF adapter to the real InputConnection through portable
epoch/revision snapshots and exact range-edit events over JNI. Full
ExtractedText replies use relative selections plus startOffset, per the
[Android API contract](https://developer.android.com/reference/android/view/inputmethod/ExtractedText).
Old-editor replies cannot replace the active document; password/number editors
destroy the TSF context and clear retained document data. Key filtering runs
before TranslateMessage; the existing IMM32/text/action path remains present.
Reentrant COM detach now retains active native procedures and retired watch
state until they return. A native fixture links the actual production IME
adapter and real TSF DLL: exact reconversion events, restricted-editor teardown,
and nested owner destruction all pass locally (zero failures, g++ Werror).
WindowInput/WindowsInputMethod and the new real EditText TsfActivity compile
against the saved framework/core classes. Both single/device CI scenarios now
exercise reconversion, composition completion, password/number exclusion and
normal-editor reconnection. These Android runtime scenarios are not yet CI
verified. Candidate-window occlusion, richer geometry/formatting and physical
IME behavior remain open; no milestone or roadmap row is ticked.

Combined WIP full CI dispatched: **40389984421**, exact source
`502b7d7a6ba7e3ca1640c5a1b8ba7a237ed107c2`, after merging current integration.
The branch merge commit was then amended only to add its required author
footer; `9d094ed` has an identical tree (empty diff against the run source).
Watch: `C:/work/gesher-ci-review/windows4-40389984421-watch.log`.
This run includes the nonce fix and TSF/COM-source/target-28 fixtures; the
previous display-only run remains diagnostic and is not cancelled. All new
runtime claims remain pending.

Post-dispatch source imports only the portable TLS lifecycle slice for the
next run: TlsServer.cs plus the local root-test server setup/cleanup in 56-net.
NetTest and the original public badssl/internet tail remain identical to
integration cd0b849; no Web3 negative-certificate helper is required. Patch:
`C:/work/gesher-ci-review/tls-lifecycle-cd0b849.patch` (SHA256
`2B975536B28F6BE8969317848CF16E3BDBADF6ED8738F747C0ED5FF0336C9629`).
Its isolated lifecycle self-test passes locally in 120 ms with the same
PFX-backed certificate setup; the C# owner itself passed both Web3 runners.
This does not change the source or assertions of either already-running CI.
A small additional TSF guard rejects service/key access immediately when the
shared editor epoch/restriction changes, before its posted window-thread state
update; the native probe covers that ordering explicitly.

The display-only CI completed both suites (20 failures on 2025, 21 on 2022),
not a TLS cleanup timeout. Its nonce failure explains the Windows3/4 rotation
crash chain and is already fixed in the combined source. It also exposed an
actual RTL regression: the newly always-present task override copied the
display's LTR screenLayout bits onto Hebrew package configurations. Geometry
overrides now leave layout direction undefined, inheriting the package locale.
A real Configuration.updateFrom dex fixture covers Hebrew/Arabic/English and
all four rotations; existing end-to-end locale checks remain unchanged.
The 2022-only old IME conversion committed three valid kanji rather than the
fixture's two-character assumption; that test's candidate selection is still
being investigated, without treating the failure as waived.

IME fixture correction prepared for coordinator review: retain real SendInput,
hiragana composition, kanji conversion, candidate/caret geometry and Enter,
but compare final all-kanji editor text exactly to the post-Enter IMM commit
event (with no composing suffix). The prior fixture accepted arbitrary
two-kanji results, including 二本, but rejected the valid three-kanji 日本語
result. It now checks actual Windows-to-InputConnection fidelity rather than
the runner dictionary's word length. This is an explicit assertion change,
not a reclassification of the old failure; runtime CI and coordinator review
are required before acceptance.
