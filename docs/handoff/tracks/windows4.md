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
job failed on the graphics-dex server dependency described below; its independent
main native build continues. Targeted framework diagnostic **40416561427** at
d50084d checks the dispatcher dependency correction without canceling that build.
The next full combined run must use the merged runtime integration above and
read actual Android IME/composition, Application Resources and legacy layout
traces. Those gaps, candidate discovery and physical touch keyboard verification
remain open; no roadmap milestone or ADR acceptance is claimed.

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
