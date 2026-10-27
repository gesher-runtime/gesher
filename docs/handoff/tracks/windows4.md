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

Current full validation **40447661700** is running at exact
`8d97726de522800bccffd994d5e91ae6d5136bbf`. It includes the focus-only fix
and real non-editor fixture below, verified clock/width integration8922a3e
and the latest single-line integration handoff46c9cc1. Corrected targeted
framework40446866229 at73ba54f succeeded; actual job122000143076 was read:
javac zero errors, app jar, graphics dex, winapp and winlegacy built.
All code in8d matches that targeted source. Root completed source review of
745fd5a and the pinned branch with no P1 finding; that is not runtime proof.
Watch: C:/work/gesher-ci-review/windows4-40447661700-watch.log, interval120.
No full-stage acceptance or roadmap tick before the actual new results.

Latest full diagnostic **40441680201** at exact
`5b5b51d0b858c6b08602af6450eed2a49025d71f` is COMPLETE RED: 28 jobs,
20 success, six skipped, two failed Windows jobs. Actual logs 121985265563
(2022) and 121985265568 (2025) each have exactly two failures: the first TSF
reconversion in single-process and device modes. All stronger native-foreground
preconditions/transitions and held/fresh shared/task Display snapshots now pass
on both runners; so do IME35, target28 legacy resize, four local TLS rejection
causes and the original gates. Actual main/framework/package and both e2e logs
are read; installed runtime/window/input/uninstall and all nine apps including
Puzzles/TIC80/four transport/association/shortcut/zero unexpected pass.

The new trace identifies the initial TSF failure: epoch1 receives two valid
ExtractedText replies (length11, selection7-11), then epoch2 binds a null
connection and disables the native document. It is not a failed initial query.
Pinned AOSP16 IMM sends WINDOW_FOCUS_GAIN_REPORT_ONLY with null editor and
connections after input has started. Its IMMS returns a focus-only/null-editor
result without calling startInputUncheckedLocked. Gesher incorrectly called
Host.startInput for that notification and erased the live editor.

The next source preserves the host binding for null-editor reports, returns
SUCCESS_REPORT_WINDOW_FOCUS_ONLY for the same focused text-editor window (or
the existing null-editor result otherwise), and invokes onInputBound only for
an actual new session. A real-service ART regression covers preservation,
new editorless windows, real restarts, real editor updates without connections,
and headless results. The existing actual TSF/EditText/span/caret tests remain
unchanged and still must pass. No runtime fix is claimed until that CI result.
The live TsfActivity additionally focuses a real non-editor button, requires
IMM not accepting text, a failed TSF edit session and unchanged editor bytes,
then returns to EditText for the actual partial-composition/caret2 assertion.
The unit Host is therefore not the only planned editorless-input coverage.
Targeted framework40445918077 at26e73f9 caught one fixture authoring error:
the pinned restart reason is APP_CALLED_RESTART_INPUT_API. Production app
compilation had succeeded; graphics-tests compilation failed at the old
constant name. The fixture now uses the exact name from pinned IMM's
restartInput path. This failed build is not runtime evidence.

## Previous diagnostic and source preparation

Completed full diagnostic **40433828877** uses exact source
`0344cee6c70ecb8603bd7f19bdfd35a3b79fe20b`. It includes the focused Java,
activation and fixture corrections below, coordinator profile integration
9dd57f5 (verified startup merge2ba9a11) and the source-only Exec4 environment
patch. Watch: C:/work/gesher-ci-review/windows4-40433828877-watch.log,
interval120. It is COMPLETE RED: 28 jobs, 20 success, six skipped, both
Windows jobs failed. The independent Exec4 stage remains unverified until
its owner reads its CI; this combined source has all 11 exec3 PASS lines per
runner, including the formerly crashing single-process APK. No full-stage
acceptance follows from these scoped successes.
All C/C++/Java compilation and native/JVM fixtures run only in CI.

Current main build 121957654239 succeeds in 28m41, boot-image compile in
30 seconds. The actual main log confirms both imported environment objects
and JNI building, with 203 assembly boundaries/zero problems. Framework
121958450006 and its app/graphics dex/winlegacy build log were also read.
Package 121966922162 succeeds in 2m32; actual installed compiler/oatdump,
boot/runtime, HWND/input/resize and uninstall cleanup PASS lines were read.
Both actual e2e logs are now read: 121967761169 (2022) and 121967761098
(2025) pass all nine apps including Puzzles/TIC-80, four pinned-byte transport
fixtures, APK association and Start menu shortcut, with zero unexpected
failures each. Both actual Windows logs are now read: 121966922116 (2022)
has four failed tests and 121966922207 (2025) has six. Both initially fail TSF
reconversion; later reconnection/reconversion and partial composition with
caret2 pass. 2022 additionally has an expired.badssl TCP timeout plus its net
aggregate; 2025 additionally has four foreground/shared-Display snapshots.
All IME35 exact Windows-result/no-composing, real floating candidates,
responsive US layout and password checks pass in both process modes on both
runners. Target28 legacy 500/800 resize/restoration and actual Configuration/
CursorAnchorInfo/native COM/TSF probes pass on both. All Display snapshots
pass on 2022; 2025's four failures remain real failures.

Next source adds metadata-only request/reply/native-unavailable diagnostics
under the existing explicit TSF fixture environment. It does not assume the
remaining first-editor failure is binding order. Pinned AOSP16's remote
getExtractedText checks session/connection, not the client's binding-active
state; no session-mismatch/inactive warning appears in the captured logs.
The fixture also requires actual native foreground before each scenario,
explicitly establishes the new landscape HWND as foreground, and checks both
real landscape->portrait->landscape transitions. ShowWindowAsync alone does
not establish that initial host precondition. Existing text/span/geometry
snapshots, deadlines and failure oracles remain unchanged. This source still
requires CI; it does not claim the TSF or 2025 foreground defects are fixed.

## Earlier diagnostics and source corrections

Full diagnostic **40417632512** at exact cde1c79 is COMPLETE RED: 28 jobs,
20 success, six skipped, both Windows jobs failed. Both actual Windows logs
were read: 2022 job 121919469641 has nine failures (41m20); 2025 job
121919469507 has eleven (37m54). Package and both e2e results below remain green.
No full-stage acceptance or roadmap tick follows from those scoped successes.

The current failures are two final IME snapshots per runner (real composing
and converted text/candidate placement now pass), two initial TSF reconversions
per runner, legacy resize in single-process mode only and unfocused-task Display
snapshots. Windows 2022 additionally has two empty password snapshots during a
180-second WM_INPUTLANGCHANGEREQUEST/ImeBroker wait. Windows 2025 alone has an
exec3 single-process native SIGSEGV after read-mode popen; the coordinator and
exec4 peer own its investigation. Device exec3 and all 2022 exec3 checks pass.

Actual new Java evidence: CursorAnchorGeometry passes under ART on both;
later TSF reconversion and actual partial composition/caret2/live spans pass;
device target28 content correctly shrinks/restores 500/800 with IME insets300.
Single-process legacy trace instead has fitTypes/fitSides zero and full frames.
The scaled single driver lacks AppCompatCallbacks' disabled-change
setup; the next source installs the same target-SDK table used in normal bind.
WindowSession's natural-rotation/empty-bounds path also omitted full appBounds
override, leaking the shared landscape size into the unfocused portrait task;
every task now receives its full TaskConfiguration geometry.

Initial TSF binding can query before InputMethodManager accepts its binding
result and never requery an unchanged selection. The next source schedules a
document/cursor query after enqueueing that result. The IME fixture's existing
composition/inset poll now reads actual span-only completion as well as text
changes, so it can observe finishComposingText without weakening the exact
native-commit/no-composition assertion. These source corrections await CI;
no local native build or new full run has been performed.

The next native source also gives top-level WM_ACTIVATE its own event, separate
from keyboard-focus changes; WindowSession uses it for task activation while
retaining normal DefWindowProc behavior. A real HWND fixture first obtains
actual keyboard focus, then requires a separate activation event without a
focus transition. Its added counterfactual removes only that event and must
fail exactly the named assertion. Fast CI now declares six negative source
copies, preserving all prior exact counts. This native change is not yet
verified and does not establish the additional 2025 foreground case as fixed.

Targeted framework 40428458347 at d98d5a1 passed pinned javac but caught an
authoring error in the scaled driver: AppBindData has no loggableCompatChanges
field at this pinned version. The direct AppCompatCallbacks.install call takes
both arrays as expected; only the nonexistent redundant field assignment is
removed. This build failure supplies no Android runtime result. Native fast
40429258614 at 4a7ca61 separately checks the activation source/fixture.

Fast 40429258614 is now complete green. Both actual Windows logs were read:
all four canonical aggregates zero/raw FAIL zero; activation with unchanged
actual keyboard focus passes, and six counterfactuals fail exactly their
declared 1/1/4/4/1/1 assertions. Corrected framework 40429365809 at 5110c9b
also succeeds after removal of the nonexistent field; its actual log still
needs review before claiming the complete build evidence.

The next IME fixture source addresses the observed Server2022 host stall
without removing the existing CloudStore workaround: a layout HKL can change
before WM_INPUTLANGCHANGEREQUEST finishes, and toggling the IME can restart
the broker after cleanup. Within the existing 30-second layout-switch deadline,
returning to US now checks a real bounded WM_NULL response and ends a restarted
unresponsive broker through the existing cleanup. A new assertion requires
actual US layout and a responsive window before the unchanged password-input
check. This recovery behavior remains unverified until full CI. Standalone CI
also compiles the updated C# helper, without claiming that as recovery proof.

Fast 40430647514 at exact 2c6b668 is complete green and both Windows logs were
read: the new C# helper compiles, four canonical native aggregates/raw FAIL
remain zero, and the six negative copies fail exactly 1/1/4/4/1/1 checks.
Corrected framework 40429365809 at 5110c9b has also been read: javac zero errors,
gesher-app/graphics dex/winapp/winlegacy builds succeed. This still leaves the
new Android binding/legacy/foreground and broker-recovery behavior unverified.

The exec4 owner supplied a source-only live-environ patch from 9f25080, SHA-256
7f20069c6d8c120bb27a948c4a82990c90436eeefeb5160ce598bfddd368ebe8. Its hash and
git apply check match; it is imported into this diagnostic branch only, in
posix_bionic.cc and gesher_exec.cc. The APK export uses the live CRT environment
slot and both modes expose __environ; the executable's UTF-8 snapshot remains
unchanged. Exec4 full 40430496516 is still pending, and its additional tests
are not imported. This is coordinated source preparation for the known
2025 crash, not an Exec4 acceptance claim or an integration/main merge.
All C/C++/Java compilation and native/JVM fixture execution remain CI-only.

Current source includes verified runtime integration 94ccde1 (AOT/DM/StrictJarFile
and exec3), merged at 9d1445c. The media conflict preserves the integration's
canonical HTTPS mirrors and 240-second install bound alongside command tracing.
No local C/C++ build or native fixture execution is allowed.

Standalone **40415866523** at exact
`d50084d731be343bc6c8cb30756a5cc713d89c14` is complete green: llvm-mingw/Werror
build plus Windows 2022 and 2025. Both actual Windows logs were read. All four
canonical aggregates are zero with no raw FAIL. Held-COM unavailable/Disconnect
layout denial and fresh-epoch geometry recovery pass on both; the five changed
source copies fail exactly their declared 1/1/4/4/1 assertions. Negative raw FAIL
lines remain visible and separate from canonical results. This is native-only
evidence, not Android/JNI, real base/eventfd, package or e2e verification.

Full diagnostic 40413109702 at 74d1346 cannot reach Android because its framework
job failed on the graphics-dex server dependency described below; it is now
complete red, with that one failed job among 25 success/failure/skipped jobs.
Its independent main native build succeeds: the actual job 121888554943 log
shows connection/IME/text-store/text-service/input_jni compilation and
`203 assembly boundaries checked, 0 problem(s)`. This is full-toolchain build
evidence at that SHA, not Android/package/e2e runtime evidence.
Targeted framework diagnostic **40416561427** at
d50084d checks the dispatcher dependency correction without canceling that build.
The next full combined run must use the merged runtime integration above and
read actual Android IME/composition, Application Resources and legacy layout
traces. Those gaps, candidate discovery and physical touch keyboard verification
remain open; no roadmap milestone or ADR acceptance is claimed.

Further source audit found a Java geometry publication gap: CursorAnchorInfo
with neither an insertion marker nor its fallback character returned before
replacing the old measured range. The source now publishes its explicit
current character measurements (including missing entries) before any caret
fallback can return. A small independent mapping helper keeps that path out of
the server class graph. A real-framework CursorAnchorInfo/Matrix dex fixture
covers supplementary UTF-16 indices, transform/rounding, clipping, missing
characters with no caret, a newer same-text observation with no bounds and
composition end. These Java/ART checks await full CI; no local build was run.

Targeted framework **40416561427** at d50084d is now complete green (both
host-tools and framework jobs). The actual framework job log confirms javac
exit zero/zero errors and successful graphics-tests.dex, gesher-app and both
Windows4 test APK builds, establishing the class-graph correction only.
New full combined diagnostic **40417632512** uses exact
`cde1c797434401bb94db24f5281a98829d15bddf`, including verified runtime/exec3
integration 94ccde1 and the missing CursorAnchorInfo publication correction.
Watch: `C:/work/gesher-ci-review/windows4-40417632512-watch.log`, interval 120.
It is diagnostic until every required result is read; no threshold or runtime
assertion was relaxed. The independent old main build was not canceled.

On 40417632512, main build 121903818145 now succeeds in 40m12 and boot-image
compile in 29 seconds. The actual main log shows current connection/TSF/JNI
objects building and `203 assembly boundaries checked, 0 problem(s)`. Framework
121913653312 also succeeds (actual javac zero errors, graphics dex and both
Windows4 APKs built); media preparation succeeds in 46 seconds. Runtime has
started: Windows 2022 job 121919469641, Windows 2025 job 121919469507 and package
121919469612. Those jobs have no final result yet; build success does not close
the Android, package or e2e gates.

Package 121919469612 now succeeds in 2m6; its actual log confirms installed
compiler/oatdump/framework and runtime smoke, real app HWND/click/keys/wheel/
resize, and uninstall device/data/association/AppIDs/shortcuts cleanup with the
unrelated Start menu sentinel preserved. Both e2e jobs also succeed and their
actual logs were read: 2022 job 121920263609 (8m46), 2025 job 121920263622
(10m33), all nine apps including Signal Generator/Puzzles/TIC-80, all four
transport cases, association/shortcut and zero unexpected failures on each.
Both full Windows suites are still running; Android/IME/legacy evidence and
overall acceptance remain pending, without a roadmap tick.

Shared edits to review at final handoff (relative to integration 94ccde1):
- framework.yml adds the winlegacy build/copy lines; graphics.yml builds and
  collects the three full-native probes and test-service DLL. The separate
  windows4-probes workflow owns only the fast Gesher fixture path.
- cmake/aosp/input.cmake and window.cmake register production TSF/COM sources
  and native probe targets; no global CMakeLists or main CI orchestration edit.
- android/app/GesherAppDriver and GesherAppProcess install the fresh Binder
  display path before Application code. The driver also publishes rotation and
  explicit zero overrides; ActivityTaskManagerService sequences focused-task
  display changes. Services connects the Binder display/callback contract.
- LocaleManagerService samples shared geometry with its configuration sequence
  and installs the initialized Runnable dispatcher for real process changes.
- InputMethodManagerService now notifies its host after enqueueing the client
  binding result; the scaled GesherAppDriver installs the same target-SDK
  compatibility table as normal ActivityThread binding. The source-only Exec4
  import additionally changes posix_bionic.cc and gesher_exec.cc as documented
  above; the coordinator owns its separate verification.
- tests/windows/tracks/35-ime.ps1 retains SendInput, actual composition/candidate
  observations and Enter. The coordinator-approved assertion correction compares
  the exact post-Enter all-kanji editor with the independently logged native
  commit, replacing the incorrect fixed two-kanji criterion. Floating candidate
  observation checks zero edge insets and unchanged client/content dimensions.
  Both Java IME fixtures expose the actual editor spans/state used by that oracle.
Portable TLS/media and quoted-launcher changes are already in the integration
base, so they introduce no remaining Windows4 delta against it.

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
`C:/work/gesher-ci-review/windows4-40377891527-watch.log`. Complete red:
2025 has 20 failures, 2022 has 21; only each runner's initial portrait W4
snapshot passes before the nonce crash. The actual package install/run/input/
uninstall cleanup passes. Each e2e has eight passing apps and Signal Generator
failing on the same display nonce path. Both public badssl/live-internet
network groups pass; neither suite times out.

The first run's asm-port job failed before its checks on upstream promisor
fetch HTTP 502/expected-packfile errors (job 121765852176, exit 123). The run
has five failed jobs in total and cannot establish a fully green milestone.

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
but compare final all-kanji editor text exactly to the post-Enter Windows commit
event (with no composing suffix). The prior fixture accepted arbitrary
two-kanji results, including 二本, but rejected the valid three-kanji 日本語
result. It now checks actual Windows-to-InputConnection fidelity rather than
the runner dictionary's word length. This is an explicit assertion change,
not a reclassification of the old failure; runtime CI and coordinator review
are required before acceptance.

The real Microsoft candidate window already inspected by the SendInput fixture
now has an explicit Android insets check while it remains visible: candidate
geometry is floating above the client's bottom; a new composing-state snapshot
must show zero IME edge insets and an unchanged decor/client size. This tests
Android's actual floating-candidate behavior without inventing a keyboard-sized
inset. CI remains pending, as do arbitrary third-party candidate discovery and
candidate-only content/visible-inset distinctions.

The coordinator reviewed and approved the explicit IME fixture correction at
0507181: the anchored final text also rejects a composing suffix, and the
post-Enter mark prevents reusing an old result. Wording now says Windows result
because the shared commit route can carry TSF as well as IMM32.

40389984421 is superseded by the new combined source (RTL fix, portable TLS
lifecycle owner and IME fixture follow-ups). The coordinator authorized its
cancellation as superseded source; package/Windows jobs had just started, so
it provides no full runtime summary. Its completed builds/media remain
diagnostic evidence only. Media logs show a silent 19:56:05–20:24:18 gap
followed by assets timestamped 20:24; no per-command timing proves exactly
where the wait occurred. The media track's independently CI-exercised slice
now conditionally skips apt when ffmpeg exists, bounds apt retries/timeouts,
limits preparation to ten minutes and prints the version. Generators/assets
and assertions are unchanged. Next: one new combined full CI, not a rerun of
the same source looking for a different runtime outcome.

Independent exec3 review found a P1 owner-lifetime gap in ApplyDocument:
unlike the key/Pump/test-service routes, Focus/Update used the member
shared_ptr without a local lease. A COM OnTextChange callback could detach
and release the service while TextStore::Apply was still running. Both
routes now retain a local owner and Focus checks detach before more work.
The native IME fixture installs an actual COM sink over the active store,
detaches from OnTextChange, observes the wrapper through a weak reference,
and proves release happens after the callback returns. No fixture-held
strong wrapper reference can hide the regression. A native-only weak
inspection method and friend access expose no Java or environment hook.
The pre-fix Update variant fails exactly that one ownership assertion;
the fixed production adapter has zero failures locally under g++ Werror.
This fix is newer than CI 40396019963 (source ed94b87); that source cannot
be treated as mergeable even if its runtime checks pass.

Follow-up independent review found no established standard-IME feedback loop,
but the COM contract permits layout -> identical selection -> host caret ->
layout feedback. Equal carets and identical selection/direction now produce
no redundant notification/edit. A real sink closes this loop and observes
one notification, while a changed reverse selection still emits exactly once.
No claim about a physical IME looping is made.

Publication across Java/native threads is now explicitly ordered by editor
epoch. finishInput captures its epoch under the editor lock; the native state
rejects older publications and a document snapshot must match the current
epoch. A late reply cannot replace a newer snapshot waiting in the queue or
disable the new editor. Real native adapter fixtures cover delayed finish,
delayed snapshot, and a fresh connection restarting epoch numbering. Both
native TSF probes pass locally with g++ Werror; Java syntax compiles against
the saved framework/core. These follow-ups and integration 117629e are newer
than the running ed94 CI and require fresh combined verification.

Recovery verification at b215be8: both current native TSF probes were built
again from their production sources with local g++ -std=c++17
-Wall -Wextra -Werror and completed with zero failures. This includes the
weak-owner OnTextChange detach regression, editor-epoch publication ordering
and the real COM layout/selection/caret feedback fixture. The Windows4
PowerShell wrapper parses. Independent static re-review confirms the 3634e97
owner lease closes its original P1; review of 106dcdd is pending. The older
ed94 CI has successful builds, media, package and both e2e jobs while its two
Windows suites are still running. These are partial diagnostic results only;
no new runtime or roadmap claim is accepted before final combined CI.

Further independent review produced a second real COM regression: an
OnTextChange callback replaced the editor while the old Update lease was
active, and the old service's later destructor overwrote the new HWND
association. The own native reproducer fails exactly that assertion on the
pre-fix source. Services now retire their owner callbacks and restore their
association before replacement, with final context/manager/DLL release still
deferred until active leases return. Reentrant document work is held across
retire/attach/update and posted once after the outer call, preserving the
latest epoch/snapshot instead of losing consumed queued messages. The fixed
real-COM fixture has zero failures locally under g++ Werror; CI is pending.
Review also identified the separate normal-HWND-destruction owner cleanup
path, which is being checked before final dispatch.

The normal destruction leak is also reproduced: DestroyWindow followed by
connection release left the Impl owner reference forever, preventing watch
storage from being deleted. A standalone fixture observes the exact owner's
deallocation without retaining it or adding a production runtime hook; the
old destructor fails that assertion and all other checks pass. WM_NCDESTROY
now publishes an atomic destroyed flag after removing the subclass/property.
Connection release drops its owner reference after known destruction (also
safe after HWND reuse), while active procedure leases protect outer calls.
The hung/live foreign-subclass fallback still retains its reachable owner.
The fixed native fixture reports zero failures locally; full CI is pending.

Independent re-review of 7def3f5 and 80cdf80 found no further actionable issue
in those corrections (static review; the peer did not run the fixtures).
The insets audit then reproduced a separate floating-pane defect: intersecting
middle/narrow rectangles were treated as bottom insets. Only a rectangle
spanning the client's full bottom edge now produces edge rows. Other visible
overlaps retain IME visibility with an empty source frame, so Android reports
zero edge insets and does not resize legacy content. The real COM-source
probe adds undock/redock cases; the target-28 scenario adds a visible floating
pane transition in both process modes. The local production-window/COM probe
fails exactly its two new geometry assertions before the fix and has zero
failures after it under g++ Werror. That local harness disables only eventfd;
the real HWND, handler callbacks and condition-variable event queue run. CI
uses the real Gesher base/eventfd path. Java/runtime verification is pending.

Diagnostic 40396019963 is complete red at ed94b87: 20 jobs succeed, six skip,
both Windows suites fail (14 checks on 2022, 12 on 2025), without a suite
timeout. Package and both nine-app e2e jobs pass, including Signal Generator,
Puzzles, TIC-80, four transport fixtures and zero unexpected failures.
The Windows failures establish actual remaining work: Microsoft TSF composing
spans/commit fidelity and caret geometry, the native COM probe's runner
geometry, Application Display real-size rotation, legacy adjustResize and
quoted Launcher command-line tokens blocking all three device scenarios.
All native ACP/session/production-IME probes, layout-direction configuration
and single-process EditText reconversion pass on both runners. The corrected
source is newer than this run; no failed assertion is waived.

The COM geometry failures led to a local native root reproducer: requesting
a 400x4000 top-level client returned 400x1061 because CreateWindowEx with
CW_USEDEFAULT clamps its initial size to the desktop work area. Android's
later native-window setup resized it, but the standalone COM source had
already queried the smaller initial client. Window creation now reapplies
the exact requested client size before publishing creation or advising COM;
initial desktop clamping is not queued as a user resize. The real HWND/COM
probe adds a larger-than-desktop client and initial-event checks; the local
condition-variable harness passes under g++ Werror. Its eventfd transport is
still CI-only. Runtime CI must establish the runner geometry correction.

Imported exec3's portable 5b772b7 quoted-Launcher fix as 3a03f40. Its pure-Java
13-case parser fixture passed locally and in exec3 CI 40405306826's build job;
actual Windows4 device/ART scenarios remain pending.

The failed Application real-size snapshot is explained by the pinned
Display bytecode: getRealSize/getRealMetrics prefer their Resources' maximum
bounds even when Binder DisplayInfo has the new rotation. Shared-display
rotation now uses the standard process configuration dispatch (with a new
configuration sequence), in addition to Display callbacks. The existing
per-Activity task overrides retain each visible task's own bounds/rotation;
the package's locales remain authoritative. The configuration fixture adds
Application max-bounds/direction checks at every rotation. Actual held/fresh
Display behavior and full locale/lifecycle regression CI remain pending.

The shared-display geometry snapshot is read under the same configuration
sequence lock, so an older concurrent snapshot cannot inherit a newer
rotation change's sequence number.

The real-IME failures exposed actual TSF composition loss: every ACP SetText
was sent through InputConnection.replaceText, which commits/removes composing
spans. A composition transaction now carries its complete composing text and
prior UTF-16 range through the existing ordered native bridge, and Java uses
setComposingRegion followed by setComposingText. Partial edits retain the
whole composition; query-only insertion cannot change its bounds. OnEnd
carries the final Windows-side document substring, so the existing post-Enter
exact-commit oracle compares an independent native TSF result with the editor.
The native ACP fixture covers supplementary text, nonzero ExtractedText
offsets, partial composition updates and the final native commit. The real
Windows service/production-IME probes still pass locally under g++ Werror.
Actual Microsoft composing spans and candidate geometry need fresh CI; the
existing real-IME assertions and thresholds are retained.

CI-only native validation is now mandatory; no new local C/C++ compilation
will run. All local before/after observations above predate that rule. The
standalone windows4-probes workflow compiles only Gesher-owned sources with
the pinned llvm-mingw and production Werror/no-exceptions flags, then runs
the real COM/TSF fixtures on Windows 2022 and 2025. It has independent
concurrency and does not fetch/build AOSP. Its window fixture explicitly
uses the production HWND/COM/condition-variable queue with eventfd disabled;
the full CI still validates real gesher-base/eventfd and Android integration.

Prepared source, not yet verified: composing-character geometry now comes
from actual CursorAnchorInfo bounds, with editor epoch and exact text
identity. The ACP store unions measured character rectangles, maps them to
screen coordinates, clips them and hit-tests actual characters; missing or
stale measurements return TS_E_NOLAYOUT. No invented caret rectangle stands
in for a text range. CursorAnchorInfo work also rejects old editor epochs.
The native fixture adds measured extents/hit testing and stale-text cases.
Replacement limits now include the ExtractedText offset without overflow.

Independent review found a partial-composition cursor mismatch in 451a41b:
replacing the middle of a composition published its full text through
setComposingText (which moves to the composition end), while native ACP had
placed the caret after the inserted substring. The corrected source emits
an additional ordered selection transaction only when needed, with revision
capacity checked before mutation. A native partial-edit fixture is prepared;
actual InputConnection coverage and CI results remain pending.

Standalone **40409646065** at
`e408f423cb13d64a1008a478531020cced3abd16` completes all three jobs successfully.
The actual llvm-mingw 23.1.3/Werror/no-exceptions build and both Windows logs
were read. All COM geometry/initial-size/source-lifetime checks and ACP
composition/measurement/hit-test/offset/partial-caret checks pass on both.
Each of the three native aggregates reports zero failures on both runners.
This is scoped native proof, not full Android/base/eventfd or package/e2e proof.

Log review also caught a misleading raw helper FAIL inside the intentionally
interrupted native detach case, despite its zero probe aggregate. The next
source tightens this: that one fixture invocation explicitly declares an
expected disconnect using a native-only DLL export scoped by RAII, still
returns a failed HRESULT, and the probe requires that failure together with
actual owner destruction and procedure restoration. All other helper failures
remain FAIL; both standalone and full-CI native wrappers reject any raw FAIL
even with an aggregate zero. Independent review agreed with that scoped
negative-case treatment. This correction needs its next CI result.

The real EditText/Windows-service fixture now also prepares an abcd composition,
edits only [1,2] to X and ends without a helper SetSelection. The editor must
finish as aXcd with caret 2, and both normal reconversion and the partial case
must expose actual live composing spans before completion. These new Android
assertions and the fixture service extension have not run in full CI yet.

Standalone 40410640598 at 20672b1 built successfully but both Windows jobs
failed before running any fixture: the strengthened PowerShell condition
placed a continuation operator at the next line's start. That authoring
error is corrected and both PowerShell wrappers parse again. It supplies
no runtime result for the new negative-case/service fixture behavior.

Further review found that caret publication still lacked its editor epoch.
SetCaret now carries the epoch through Java/JNI/native, rejects stale input
under the shared state lock and clears old caret state when the editor
changes. A real GetCaretPos fixture publishes a new-editor caret followed
by a delayed old one and requires the new position to survive. CI is pending.

Legacy adjustResize still needs diagnosis, rather than an unconditional
fitInsets override: the pinned ViewRootImpl already applies the compatibility
policy before add/relayout, including explicit fit controls and layout flags.
The next combined source logs actual IME layout attrs, requested/frame/parent
and native client sizes at source changes. Its existing 500-pixel content
assertion stays intact while locating the missing propagation.

Standalone **40411795761** at
`828dc874538ab51ed35e4671776a618278583656` is complete green: the pinned
llvm-mingw/Werror/no-exceptions build and both Windows fixture jobs succeed.
Both actual logs were read. Each ACP, production-IME and COM aggregate is
zero, and there is no raw FAIL. The expected detach interruption is INFO
with an explicit failed-HRESULT/procedure-restoration assertion. The new
actual Windows caret epoch test passes on both runners, alongside measured
geometry, partial composition cursor, initial-size and floating-pane checks.
This verifies the native fixture scope only; the next full combined run is
diagnostic for real Android/JNI/IME, Application resources, legacy frames,
package cleanup and both e2e suites. Legacy/real-IME gaps remain open until
those actual results are read. Current integration f520fd1 is included.

Full combined diagnostic **40413109702** uses exact source 74d1346 (a
documentation-only child of native-verified 828dc87). It remains unchanged
while running. The next final source must include the coordinator's newer
runtime integration 6dc3f03 (verified AOT/DM/StrictJarFile stage), and any
subsequent verified integration before final full CI.

The standalone workflow's next source adds explicit CI-only counterfactual
copies: disabling the Update lease, caret epoch rejection, measured geometry,
composition transaction kind or initial client-size correction must trigger
exactly the named regression assertions. These are deliberately changed
current-source copies, not claims that a historical full tree was rerun.
Canonical fixtures still require exit/aggregate zero and no raw FAIL. Negative
copies must return the exact declared nonzero count and labels; their raw
FAIL lines and generated sources stay visible in CI evidence. Unexpected
failures, counts, timeouts or source-replacement drift fail the workflow.
The real Windows service also gains a native partial-composition bridge
check, and ACP geometry gains actual clipping/missing-character checks.
All of these new checks await CI; no local compilation/execution is allowed.

Counterfactual CI **40414421254** at a827d5c completes all three jobs
successfully. Both actual Windows logs were read: all four canonical native
aggregates are zero with no raw FAIL; actual Windows-service partial editing,
measured clipping and missing-character behavior pass. The five deliberately
changed source copies fail exactly their declared 1/1/3/4/1 assertions on both
runners. Those negative raw FAIL lines stay visible and are not canonical
failures or a full Android-suite result.

The full diagnostic 40413109702 fails its framework job after the pinned
framework javac succeeds with zero errors: the graphics dex fixture's new
ApplicationDisplay import followed a static dependency into the complete
LocaleManagerService/Services/CompatChanges graph, outside that fixture's
generated-server class path. The production configuration service now installs
a real Runnable dispatcher after initialization, keeping ApplicationDisplay's
configuration helper independent of the server dependency graph. No test or
assertion is removed. Native/JNI build jobs in that old run remain diagnostic;
it cannot reach the required Android/legacy observations. The corrected
combined source and latest runtime integration need new CI verification.

A final native lifecycle audit also found stale layout data on retained
unavailable/retired COM stores: Disconnect cleared text but retained its caret,
and an unavailable same-epoch snapshot retained measured text. The corrected
source clears caret/geometry at both boundaries and returns TS_E_NOLAYOUT
after the existing lock/argument checks. Independent review agreed with that
boundary. New held-COM and unavailable-to-new-epoch recovery checks are
prepared for CI, with the geometry counterfactual's additional recovery
assertion declared explicitly. No local native build or execution occurred.
