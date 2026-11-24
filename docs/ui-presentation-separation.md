# UI presentation separation

Base: `66d21c895fc0a385881446408d2a369a98336ef5`.
Local branch: `codex/ui-presentation-separation`; uncommitted changes.

| Surface | Before | After / remaining work |
| --- | --- | --- |
| Control panel and two internal WPF dialogs | Interpolated XAML mixed with orchestration | Three external XAML files own layout, styles and templates. `New-PanelView` loads them and supplies typed/literal resources. Controller still owns events, data, row view models, feedback, icon construction, severity selection and lifetime. |
| Permission, account chooser, exact alarm | Control construction inside modal callbacks | Named creation helpers own control layout/fonts. Callbacks retain data population, policy, owner checks, focus, results and cleanup. Templates for top-level dialogs remain in existing request wrappers. |
| Shared Win32 theme and messages | `host_dialog_theme.h` already owns colors, painting, DPI/high contrast and native message construction | Reused unchanged; no new shared design. |
| Launcher splash | GDI paint code inside window callback | `PaintSplash` owns drawing with a const view of existing state; window creation, resources, process observation and cancellation stay in place. |
| Android window shell | `Window::Run`, registration, WndProc and buffer/input methods | `window_frame.h` owns initial frame style/bounds, shared DPI-aware client/frame sizing, Back geometry and painting, and non-client geometry/hit testing. `Window` retains HWND/COM/thread ownership, popup/owner semantics, splash discovery/positioning, readiness, state, input/Back/close events and buffers. APK-rendered content is untouched. |
| Inno Setup | Existing packaging | Only four required resource/helper entries added. |

No new runtime, framework, shared visual design, APK behavior or migration to
C#/WPF. Native creation and default-message handling retain their ordering on the same modal/window thread.
JNI boundaries, request verification, result codes, keyboard defaults and resource
cleanup stay with their existing owners. Dynamic WPF resources carry translated
strings as objects, including within item templates; XML/markup is not generated
from translations. Window language selection remains in `gesher-ui.ps1`.

## Validation performed

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

Additional native validation (same local diff, no installed runtime suppliers):

- The coordinating chat compiled `libgesher_account_crypto.so` with the JNI
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
  CI itself has not run locally.
- Loaded all three WPF views in both languages from an isolated package-layout
  directory with spaces and Hebrew in its path, using copied source resources
  and helpers. This exercises the production loader independent of working
  directory and checks Inno manifest entries; it is not an installed Setup test.

Remaining validation: full linking/execution of the Android window stack,
ART account chooser, Android Back/close event delivery, COM/input-pane/drag/drop
lifetime, physical DPI transitions, accessibility/visual comparison, installer
build and actual installed-resource checks. The new frame fixture does not run
`Window::Run` or its event queue. Existing unchanged integration probes remain
required for those contracts. A larger extraction of registration, HWND creation,
message-loop ownership or rendering is intentionally left out: those operations
coordinate Android readiness and thread teardown rather than define presentation.
No active installation, registry or Android profile was changed. `outputs/` was
left untouched. The test/build products remain in ignored `out/presentation-review`.

## Proposed PR

Title: Separate WPF resources and Windows frame presentation from UI control logic

### Summary

The dashboard and internal dialogs embed layout in the action controller, while
native modal callbacks mix control creation with request handling. Extract their
presentation boundaries within the existing technologies without intentional
visual or workflow changes.

```text
control panel -> panel-view loader -> dashboard / confirm / message XAML
credential callbacks -> per-dialog CreateControls helpers
splash callback -> PaintSplash
Android Window -> window_frame geometry / hit testing / Back drawing
```

### Evidence

Before: XAML interpolated translations directly into controller source; native
callbacks constructed controls inline. After: production resource loading passes
for English and Hebrew, including literal strings and template construction;
existing action (219), idle (10 + 22), virtualization (60), update (101) and
wrapper/WPF interaction (251) checks pass. Translation and package-entry checks
also pass. Production permission/exact-alarm dialogs pass 500 checks; production
frame helpers pass 46 checks. The full window translation unit compiles with
warnings as errors. Package-layout resource loading passes; actual installed
validation and Windows/ART window execution remain pending as described above. No Android contract change is
intended; request/result and lifecycle logic remain in their original owners.

### Merge Danger

**Door:** two-way; source/resource refactor, no data migration.

**Blast Radius:** desktop. Missing packaged XAML prevents panel loading; native
construction regressions could affect consent dialogs or Android window bounds
and Back hit testing. Explicit installer entries,
CI installed-file checks and host resource checks cover part of that risk; native
integration and visual verification are still needed before approval.
