# Storage crash evidence at 6ff0bd78

The Windows 2022 execution passed the native exception diagnostic control and
seeded all 14 storage keys, then the actual production Host deletion process
failed with `0xC0000005`. The retained runtime DLL matches the loaded image's
PE and CodeView identity. Its exact instruction bytes place the fault in a
node-removal path reached **after an indirect callback returns**. This supports
testing deferred controller cleanup, but does not establish the complete cause
or qualify storage deletion.

Windows 2025 stopped earlier during the DLL probe's controller setup. It did
not execute the storage test or its exception control in this run. Its startup
failure must not be counted as another storage crash.

The [paired JSON receipt](webview-storage-crash-6ff0bd78.json) retains the exact
logs, image identities, instruction excerpts, source excerpts and limitations.
This report concerns ordinary stability and callback lifetime and makes no
production change. The continuation remains a draft and requests no merge or
auto-merge. Any concerns in prior security reviews remain unresolved review notes.

## Exact execution

| Item | Identity |
| --- | --- |
| Run / attempt | [45609391658](https://github.com/gesher-runtime/gesher/actions/runs/45609391658) / `1` |
| Actor | `palmoni5` |
| Source | [`6ff0bd78325e5bb2086a9f02c18d7bb091381eef`](https://github.com/gesher-runtime/gesher/commit/6ff0bd78325e5bb2086a9f02c18d7bb091381eef) |
| Source tree | `a4bbccc863326b05b730b98bf8a86213b194e901` |
| Created / updated | `2026-11-25T11:58:48Z` / `2026-11-25T12:01:16Z` |
| Build | [139188660878](https://github.com/gesher-runtime/gesher/actions/runs/45609391658/job/139188660878), successful |
| Windows 2022 | [139189279086](https://github.com/gesher-runtime/gesher/actions/runs/45609391658/job/139189279086), runtime `131.0.2903.86`, failed storage qualification |
| Windows 2025 | [139189278857](https://github.com/gesher-runtime/gesher/actions/runs/45609391658/job/139189278857), runtime `157.0.4341.46`, storage step skipped |
| Storage probe | SDK `1.0.4258.31`; static loader, `WebView2LoaderStatic.lib` |

The cached run metadata supplies the source tree. Local commit
`fd175ac545fc7fc2ae155de1132e30cdc6ef7a48` has that identical tree and was used to
check the executed source without another fetch. The uploaded probe artifact
is `webview`, ID `15018658435`, ZIP size `2,859,618` bytes. Both jobs downloaded
the same digest recorded by the build.

## What Windows 2022 actually reached

The intentional exception control now passes: log line 637 confirms that the
wrapper retained the required failing subprocess exit and actual stack. Lines
638–648 record code `0xE0470001`, flags `0x81`, the expected two parameters, a
faulting DLL and executable caller frames. The executed wrapper checks exit
`-532217855`; its success log does not print a separate numeric exit line.
This is evidence that the diagnostic works, independent of the storage result.

The fixture observes the real nested key ending in `/^31` at line 1388. The
registry inventory contains 14 distinct keys with positive usage, and the
all-written result appears at line 1436. The ordinary controller starts its
real no-store network control at line 1500. The production test then exits
`-1073741819` (`0xC0000005`): line 1502 reports `qualified=0`, 34 missing
acceptance markers and zero final callback checks.

No production callback summary, final preservation result or successful
qualification marker appears. The failure wrapper subsequently echoes the
captured output, including the seed results; those repeated lines are not a
second execution. An absent summary does not prove that no native callback
ran before the crash.

## Loaded image and retained file agree

The actual fault record is at lines 1930–1935. It identifies thread `4932`, a
read access violation, module `EmbeddedBrowserWebView.dll`, loaded base
`0x7ff9efdd0000`, and fault RVA `0x353752`. The diagnostic records the actual
loaded path beneath runtime `131.0.2903.86`; the workflow copies that file and
records its SHA-256 at line 1965.

| Image property | Loaded record and retained file |
| --- | --- |
| Machine | `0x8664` — x64 |
| PE timestamp | `0x6750fb41` |
| Size of image | `0x538000` |
| Copied file size | `5,368,400` bytes |
| File / product version | `131.0.2903.86` |
| CodeView GUID / age | `85974DE8-90A2-8FCF-4C4C-44205044422E` / `1` |
| Recorded CodeView key | `85974DE890A28FCF4C4C44205044422E1` |
| Recorded PDB basename | `EmbeddedBrowserWebView.dll.pdb` |

The copied file and manifest are retained in artifact
[`15019896217`](https://github.com/gesher-runtime/gesher/actions/runs/45609391658/artifacts/15019896217),
`webview-native-exception-windows-2022`, ZIP size `2,408,942` bytes. Both extracted
files were checked byte-for-byte against that existing ZIP. The copied DLL's
PE and CodeView values were independently parsed and matched to the loaded
records and manifest. This is an image identity match; no whole in-memory
image hash or snapshot of the runtime's private objects was captured. The PDB
name is metadata only: no PDB was used to name internal functions.

## Instruction order and its limits

The actual stack contains runtime RVA `0x3296d4` immediately above the faulting
helper. The retained PE runtime-function table bounds that caller at
`0x32927c`–`0x329731` and the faulting helper at `0x35373e`–`0x35378f`. Every
instruction byte in the three saved disassembly samples was checked against
the exact retained DLL or executable: 326 instruction sequences in total.
Nearest-export labels printed by the disassembler are not treated as function
identities.

| RVA | Direct instruction evidence |
| --- | --- |
| `0x329321`, `0x32935a` | The routine references the exact literals `id` and `method`; the matching-id branch selects a node. |
| `0x32969c` | A callback pointer has been moved to a local holder, and the selected node's field at `+0x28` is cleared. |
| `0x3296b0` | An indirect call uses a function pointer loaded from that callback object. |
| `0x3296b9` | After the indirect call returns, a helper operates on the local callback holder. |
| `0x3296cf` | The selected node is passed to the helper at `0x35373e`; its return address `0x3296d4` appears in the actual stack. |
| `0x353752` | The helper faults at `movq (%rdx), %rdx` while following links. Captured RDX is `0x4f20faffaf8cffff`; the exception record reports target `0xffffffffffffffff`. |

The helper also updates links and decrements the container count. Together
with the id lookup and callback dispatch, these bytes are consistent with a
response-callback registry that removes its node after returning from the
callback. **That internal data-structure description is an inference.** The
instruction order and actual caller return address are concrete evidence;
neither identifies the particular callback, the earlier writer of the invalid
link, or the complete cause.

The exact probe stack also contains RVA `0x5392b`. Its retained executable maps
that address to the owner thread immediately after `DispatchMessageW`. The
captured stack has no active application callback or controller-close frame;
that is compatible with a callback having already returned, and does not
exclude an earlier cleanup action.

## Focused candidate, still awaiting actual acceptance

The executed source closes its resolver and inventory controllers synchronously
inside `OriginStorageDelete::Finish`. The DevTools completion path can call
`Finish` inline. In combination with the observed post-callback runtime node
handling, this provides a specific reason to test cleanup after the SDK
callback stack has returned.

Candidate `2857fb1bc03eb4906868de0827f6a4e7f56fc231` moves terminal cleanup to
the existing owner task queue while retaining the active operation and
`GlobalWork`, including during shutdown drain. Its tree is
`a3984c23fd853b2d651b50f0888395bee1bf42a1`. It has **no actual Windows acceptance
result in this receipt**. Compilation or isolated scheduling checks cannot
replace the unchanged actual Host fixture's deletion, preserved-data,
callback, concurrent-network, browser-exit and shutdown-drain gates.

## Windows 2025 is a separate startup failure

Lines 153–160 identify the DLL loader and runtime 157, followed by controller
setup failure or timeout. Its late controller completion returns `S_OK` at
line 161, and late `Close` returns `S_OK` at line 164. The DLL probe still exits
1 at line 185. The subsequent static probe creates its controller at line
195, but the already-failed prerequisite step causes live storage qualification
to be skipped. The cached job metadata confirms that skip, and this log has no
storage execution or native exception-control record.

## Durable evidence identities

Log anchors are one-based physical lines, including blank lines. Each complete
saved log was checked against its cached tool-result content. No additional
API read or download was performed to prepare this report.

| Evidence | SHA-256 |
| --- | --- |
| Build log: 344 lines, 27,417 bytes | `04683a722e22d713dd3fb6c4e4378a040cd0c6988e8c2e3747ce7f1f5b7236c7` |
| Windows 2022 log: 2,526 lines, 275,944 bytes | `3ec632963e9eaae41ecdecd55163fc867d4fde0720f50f4915508d8fbba062f9` |
| Windows 2025 log: 261 lines, 24,626 bytes | `37a7de2c9936d88b87a9d7c6a49084c592ae199a07e148873933d3015034e23d` |
| Probe artifact ZIP | `915d4daaec7bfb4048d6c406aeba2eea7b30c56ca55d76a517a1a9052cd2fb77` |
| Exact `webview2_probe.exe` | `e8edcb67d76843ee4128e9e2e8f76875da0a74a1ca6e98f9d05c4b6d1debde14` |
| Runtime evidence ZIP | `dfd64e7f779fdb6fe0e131dba99bca4dd2d95d64f221e7b3cfe2b31439266fb7` |
| Exact `EmbeddedBrowserWebView.dll` | `fa7b92b1895be8ee165c08db635143181aa030a505fc6567fd7978171e1550d7` |
| Runtime manifest | `58d588e62b7008a318ca0e171350486faba86c2ced3aec872a7b1a4b2018c160` |
