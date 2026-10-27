# Remaining process-helper source inventory

Source checkpoint6dd5cc88,2026-10-27. This is source inspection, not a claim
that any specific application failure is caused by these waits. The actual
AST-derived29 zero-argument WaitForExit sites are saved in
C:/work/gesher-ci-review/ci-process-legacy-zero-arg-wait-inventory.json.
RunTimed/test50 has scoped40526183946 proof; actual SurfaceRun/test52 and
InvokeOpenSl/test76 corrections await changed-source40552781028 after the
retained frozen-helper fixture failure in40551845130.

| Source | Function / site | Distinct behavior to preserve and test |
| --- | --- | --- |
|20-libs,21-posix|Invoke-CoreLibsTest,Invoke-ArtGtest|Real gtest output/status, timeout and child-held output|
|30-graphics|GraphicsDex|Async redirected readers and native exit status|
|34-windows,35-ime,36-popups,37-webview,41-native-activity,48-lifecycle,51-windows3|WindowAppCheck,ImeAppRun,PopupAppCheck,WebAppCheck,NativeActivityCheck,LifeAppCheck,W3Run|Original UI/deadline/pixel assertions, WM_CLOSE, real fresh process exit|
|37-webview|script-level installer/probe wait|Original actual process identity and result|
|38-media|RunVideoApp|Original video/pixel timing and timeout evidence|
|44-native2|N2Run(two sites)|Actual two-ABI ELF/JNI/APK status/output and two timeout paths|
|45-exec3,67-exec4|InvokeExec3Timed,script native environment control|Original shell/exec statuses and captured diagnostics|
|47-arm64|A64Run|Existing actual translated process and status contract|
|49-locale,50-services|LocaleWait,WaitTimed|Start/wait split, true native status, async stdout/stderr completion|
|54-web3|Web3Run|Original60+60/400s and fresh child completion; preserve new independent observer before changing this baseline|
|54-windows4|W4NativeProbe|Native counterfactual counts, stdout/stderr completion|
|56-net|NetDex,NetRun|Real external/local network outcomes and bounded readers|
|58-devices|DevRun|Real device/camera/geo output and timeout, preserving prior bounded probe|
|60-boot-image|Startup,script trace,CompiledCount|Original startup marker/performance thresholds and independent code count|
|61-aot2|Aot2Startup|Original startup timing/mode identity and output|
|71-media4-format|InvokeMediaFormat|Native semantic results/status and redirected output|

Each next slice must preserve original primary deadlines and failure meaning,
cover genuine distinct subprocess behaviors in CI, and retain raw failures.
Do not blanket-replace the helpers or infer that a textual pattern alone
proves an observed hang. This table does not exhaust blocking pipe reads,+task waits or helpers outside the tracks directory; those remain in scope.
Job-object containment and actual injected kill denial remain unproved.
