# UI presentation separation

Presentation is separated within the existing WPF and Win32 technologies.

| Surface | Before | After / remaining work |
| --- | --- | --- |
| Control panel and two internal WPF dialogs | Interpolated XAML mixed with orchestration | Three external XAML files own layout, styles and templates. `New-PanelView` loads them and supplies typed/literal resources. Controller still owns events, data, row view models, feedback, icon construction, severity selection and lifetime. |
| Permission, account chooser, exact alarm | Control construction inside modal callbacks | Named creation helpers own control layout/fonts. Callbacks retain data population, policy, owner checks, focus, results and cleanup. Templates for top-level dialogs remain in existing request wrappers. |
| Shared Win32 theme and messages | `host_dialog_theme.h` already owns colors, painting, DPI/high contrast and native message construction | Reused unchanged; no new shared design. |
| Launcher splash | GDI paint code inside window callback | `PaintSplash` owns drawing with a const view of existing state; window creation, resources, process observation and cancellation stay in place. |
| Android window shell | `Window::Run`, registration, WndProc and buffer/input methods | `window_frame.h` owns initial frame style/bounds, shared DPI-aware client/frame sizing, Back geometry and painting, and non-client geometry/hit testing. `Window` retains HWND/COM/thread ownership, popup/owner semantics, splash discovery/positioning, readiness, state, input/Back/close events and buffers. APK-rendered content is untouched. |
| Inno Setup | Existing packaging | Five required entries added: the action helper, view loader and three XAML resources. |

No new runtime, framework, shared visual design, APK behavior or migration to
C#/WPF. Native creation and default-message handling retain their ordering on the same modal/window thread.
JNI boundaries, request verification, result codes, keyboard defaults and resource
cleanup stay with their existing owners. Dynamic WPF resources carry translated
strings as objects, including within item templates; XML/markup is not generated
from translations. Window language selection remains in `gesher-ui.ps1`.

## Local validation performed

- Windows PowerShell 5.1 STA: `ControlPanelPresentationChecks.ps1` passed:
  three production XAML resources loaded in English and Hebrew; title resource,
  literal braces/ampersands/newlines/tabs, item-template construction,
  confirmation style and message cancellation properties; all package scripts parsed.
- `ControlPanelActionChecks.ps1`: 219 checks passed, including 64/64 failed-start
  handles closed and negative controls.
- `ControlPanelLaunchChecks.ps1`: 7 checks passed after adapting its row-template
  assertion to the external dashboard XAML. It retains the localized Launch
  control, package and visibility bindings, and the installed-package state checks.
- `ControlPanelIdleChecks.ps1`: 10 counter checks and 22 WPF lifecycle checks passed.
- `UiLocalizationChecks.ps1`: translation/language/literal-path checks passed.
- `ControlPanelVirtualizationChecks.ps1`: 60 checks passed with 1,000 items,
  English/Hebrew and two heights; 3–4 realized containers.
- `ControlPanelUpdateChecks.ps1`: 101 checks passed.
- `Issue32And33Contracts.ps1`: 251 production wrapper/WPF checks passed,
  including confirm/cancel/close and literal multiline message interaction.
- Presentation checks also verify required files and explicit installer entries.
- `git diff --check` passed. Source review covered moved control/paint code and CI lists.

Additional native validation (before the CI run below, without installed runtime suppliers):

- Local validation compiled `libgesher_account_crypto.so` with the JNI
  headers and credential includes, `gesher-launch.exe`, and
  `permission_dialog_test.exe`. Its retained log in
  `out/presentation-review/permission-test.log` records **500 checks, 0 failures**
  for real production permission/exact-alarm dialog code on an isolated desktop.
- Compiled production `gesher_window.cc` to an object with the pinned
  llvm-mingw 20261006 compiler, C++17, `-O2 -Wall -Wextra -Werror`, Windows 10
  definitions and the production POSIX/Windows include directories.
- Compiled and ran `window_frame_test.exe`: **46 checks, 0 failures**. It calls
  production frame helpers on real top-level/popup HWNDs on a never-displayed
  desktop, checking exact client sizes, Back/bar/client hit testing, initial
  style/position and bounds. DIB rendering at 40/60/80-pixel bar heights covers
  idle/hot/pressed colors and triangle drawing; 900 repeated paints retain no
  extra GDI objects after stock-object warmup. These heights model rendering
  scales, not physical multi-monitor DPI transitions.
- The first GDI census included lazy stock-object initialization and failed;
  initializing those objects before the census made the resource check measure
  repeated rendering rather than first-use cache allocation.
- Added `window_frame_test` to the native CI build and bounded Windows contract
  track, and package-directory WPF loading after the disposable CI installation.
  Full CI subsequently passed as recorded below.
- Loaded all three WPF views in both languages from an isolated package-layout
  directory with spaces and Hebrew in its path, using copied source resources
  and helpers. This exercises the production loader independent of working
  directory and checks Inno manifest entries; it is not an installed Setup test.

## Completed CI validation

The reviewed head `6ca4e6b693bc50bfadcef731e193616bbdf68132` passed
[full CI, attempt 2](https://github.com/gesher-runtime/gesher/actions/runs/45339730245/attempts/2)
and [ARM64 Windows](https://github.com/gesher-runtime/gesher/actions/runs/45339729822).
The [independent review](https://github.com/gesher-runtime/gesher/pull/422#pullrequestreview-6036593638)
and [completion update](https://github.com/gesher-runtime/gesher/pull/422#issuecomment-7127356492)
record the results and their limits. These results apply to that commit.

Windows 2022 and Windows 2025 checks covered external WPF views in both
languages, native consent dialogs, frame geometry/drawing, Windows/ART
windowing, launcher/services and end-to-end execution. Installed-package
checks covered resource loading, application interaction, splash handoff,
136 portable-runtime checks, upgrade/data preservation and uninstall.
Virtualization checks reached 64 assertions in CI; the earlier local run
recorded above passed 60. Installer and portable packaging include both
helpers and all three XAML resources.

## Validation limits

Physical cross-monitor DPI transitions, the complete pointer-click-to-Android
Back sequence and pixel-exact spinner appearance were not tested. Back-key
integration and frame hit testing/drawing were exercised separately. The frame
fixture does not execute `Window::Run` or its event queue; Windows/ART integration
checks provide separate evidence for window and lifecycle behavior.

A larger extraction of registration, HWND creation, message-loop ownership or
rendering is intentionally left out: those operations coordinate Android
readiness and thread teardown rather than define presentation.
