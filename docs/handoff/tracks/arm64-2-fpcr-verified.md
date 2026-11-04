# arm64-2: verified FPCR rounding slice

Full source `49ade2057a9d2f4e167f4a376467d9607adaf2d4`, full ci
[40451978087](https://github.com/gesher-runtime/gesher/actions/runs/40451978087),
attempt 2. All 28 job conclusions were personally read: 22 successful and
six skipped. All 22 actual successful job logs were read under the current
attempt's job IDs. This accepts translated FPCR rounding only; arm64-2 and
the rest of the floating-point environment remain open.

## Runtime evidence

| Gate | Attempt-2 job | Actual result |
|---|---|---|
| Windows 2022 | 122027516295 | New payload: `0 test(s) failed` at 2026-10-27T05:54:37.7287580Z |
| Windows 2025 | 122027518275 | Retained payload: `0 test(s) failed` at 2026-10-27T05:25:39.2343419Z |
| Package | 122027516759 | 23 actual installer PASS lines |
| E2e 2022 | 122027517640 | Nine apps, four transport cases, association/shortcut launches; `0 unexpected failures` |
| E2e 2025 | 122027518250 | Same gates; `0 unexpected failures` |
| Actual APK build | 122027515401 | Extracted ARM library byte/definition/undefined-symbol audit passes |

Both Windows runners print all five test69 PASS lines, including:

    PASS  arm64-2: real FPCR rounding, nested callbacks, thread isolation and restored native host fenv
    PASS  arm64-2 FPCR (device): pinned modes, actual arithmetic, bits, pthread inheritance/isolation and nested callbacks
    PASS  arm64-2 FPCR: an actual native ELF process verifies pinned rounding, threads and nested callbacks

The other two FPCR cases consume the actual ARM dalvikvm and APK
single-process reports. They share those same process logs with the six
test66 x86_64/arm64 dalvikvm/single/device cases; no additional Activity run
stands in for the claimed process. All six wide cases pass on each runner.
Both runners also pass signed/unsigned whole-range width digests, the 33
boundaries, all four Native2 APK checks, ART CPU clocks and test68's patched
QPC/UTC/deadline clocks. The unchanged image-size and startup comparisons
pass, with the shipped image 75.3 MB versus 162.7 MB.

Package verifies installed oatdump/image/fonts, install-time dexopt,
associations/shortcuts, real click/keyboard/WM_CHAR/wheel/resize input and
complete process/data/association/toast/Start-menu uninstall cleanup.
Both e2e payloads verify all nine apps, including required Puzzles and
TIC-80 frames/input/process exit, all four pinned APK transport cases,
association and shortcut launches and the explicit zero summary.

Attempt 1 had 21 successful jobs, one failed Windows job and six skips.
2022's only two counted failures were the outer and inner checks for
`untrusted-root.badssl.com/104.154.89.105:443` timing out during TCP connect
after 15000 ms. Only the failed job was rerun on unchanged source. Its
new log actually rejects the untrusted root with SSLHandshakeException
at 05:46:24.1930651Z and finishes with zero failures. The other 21 payloads
retain their attempt-1 timestamps and were reread under attempt-2 IDs;
they were not rebuilt or rerun.

## Implementation and independent reference

The real dynarmic GetFpcr/SetFpcr backend supplies ARM fesetround modes
0..3 and fegetround, preserving other FPCR bits. Invalid modes return -1
without state or errno changes. Direct guest MSR changes are visible.
Each nested guest entry inherits the actual parent FPCR and writes its
final value back; the outermost state persists per thread. A narrow
pthread_create wrapper captures the guest FPCR for the new thread.

Host calls and raw syscalls are queued, leave Jit::Run at the saved PC,
execute in the native host environment and resume through a fresh dispatch.
This avoids an old block's FPCR assumptions and keeps arbitrary native
callbacks outside guest MXCSR. Native fenv guards restore the host after
entries, callbacks and raw syscalls. Existing guest TLS layouts, common
pthread code and loader interfaces are unchanged.

The Windows host fixture uses actual NDK-built MSR/MRS and positive/negative
halfway FADD, all modes, preserved DN/FZ bits, invalid modes, three guest
nesting levels, concurrent state and native fenv restoration. Shared actual
NDK contracts add FADD/FRINTI, import/dlsym entries, pthread inheritance and
four concurrent worker modes, and nested qsort callbacks. JNI, both APK
modes and a real native ELF process execute those contracts.

Full licensed android-16.0.0_r4 fenv-arm64.c and its ARM header are saved in
`probes/arm64/fpcr/upstream`. Their original function bodies compile as
real guest instructions with symbol names changed for coexistence. They
are not a simulated register or a copy of the shim. Source manifests and
actual-tag byte checks retain provenance. The last scoped correction run
[40451472255](https://github.com/gesher-runtime/gesher/actions/runs/40451472255)
at e5789e3e passes all five executed jobs: source provenance, -Werror
translator/NDK builds, actual packaged reference audit and both Windows
host fixtures with all four FPCR groups and zero failures.

Initial full 40444486511 at e0f9601 failed because the separate APK builder
omitted the original-body reference object while recompiling its library;
both APK modes called unresolved reference symbols. The corrected builder
links that object, extracts the actual APK library, compares its bytes,
checks both rounding definitions and rejects any unresolved reference name.
Production code did not change for that correction. That earlier run also
had one 2025 media frequency failure; no audio gate was changed or waived.

## Prerequisites, shared edits and limits

This source includes integration 7694f183 (docs above verified 8922a3e),
root's corrected signed/unsigned width and 0572/0573 CPU/elapsed clocks,
and the verified wide/C11 code, retaining both GuestLocaleconv and width
adapters alongside the new FPCR/pthread adapters. It predates newer Media3
and exit changes. Root must run coherent combined CI when merging a
different non-documentation tree; this report does not prove that tree.

FPCR production changes stay in compat/arm64. Shared test/build edits:
Native2's source/header wiring; arm64_abi_test.c and dedicated native
assembly/process contracts; test44's additional evidence list from the same
actual logs; reserved test69; native2.yml's reference object and APK sparse
paths; native3-wide.yml's ARM reference link; native2app/build.sh's actual
APK reference link/audit. The reusable ARM workflow builds the host fixture
and process artifacts; the branch-only arm64-fpcr workflow tests them.
Documentation and the scoped roadmap row are additive. Inherited wide
inventory metadata remains scoped; historical counts and owners stay intact.

FPSR sticky exceptions, full environment save/restore, the ARM default
environment object and exception/trap contracts are not accepted by this
slice. In particular, rounding-sensitive native host CRT/libm calls,
including the narrow converter used by wide float parsing, are open under
non-default guest FPCR modes. The wide reference proves grammar/end
handling rather than a complete independent Android gdtoa implementation.
Guest JIT pages, signals/ucontext, LSE, HFA/variadic binary128, code-cache/
stack bounds and cleanup remain open. No local C/C++/Java compiler or
native/JVM fixture ran; all compilation and execution were CI-only.

The immutable `wip/arm64-2-fpcr-verified` ref preserves the documentation
checkpoint above this exact source. The earlier wide frozen ref stays at
e7c1b06. Root alone merges integration/main and publishes releases.
