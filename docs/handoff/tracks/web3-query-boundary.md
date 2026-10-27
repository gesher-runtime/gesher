# Web3 native query boundary source checkpoint

Status: sourcefc176703 verified in component40577659058 on both Windows hosts.
See web3-query-component-proof.md for actual logs and precise limits.
Earlier WIP sections below are historical; canonical default-OFF/full/package/e2e remain open.
Dedicated wip/web3-query-bound/C:/work/gesher-web3-query-bound starts with
accepteda7889e76 plus exact component-proven Native frozen db9fa9ad.
It does not import the unrelated AOT96b diagnostic ancestry.

Original failure40550620230/f4b, Windows2025 job122353154161, records the
A11y60+60s failure before bounded native observation. Exact original-module
analysis40561026137 and40562238173 proves app7400 main2304 in Window destructor
join, owner6092 in ART monitor entry, WebView STA8796 in ThreadMain+0x228.
The exact historical Java monitor/owner is not captured. WindowSession.remove
releases mWindows before nativeclose; do not claim otherwise.

## Implemented first source slice

query_pool.h/.cc defines four native workers and32 queued requests, one
absolute3s deadline before any ScopedEnv/JNI handoff, bounded admission,
single-use shared native payloads/window epochs and bounded nested rejection.
Expired queue requests are pruned; no pool/request lock crosses callbacks
or JNI. A completed result is published after the callback has returned and
must already have destroyed/detached its ScopedEnv. Pool objects are process-
owned, with no static-destructor join. Stop rejects admission, cancels queued
and active work, drains callbacks and joins workers within its explicit3s.
Lifecycle serialization prevents concurrent-stop double close; stop-before-
start is terminal; partial startup cleans created handles with a bound.

## Remaining before runtime qualification

- Wire real four UIA wrappers before ScopedEnv, deep result conversion and
  per-window generation/retirement before the real native destructor join.
- Carry cancellation into Java root/viewRoots/hwndOf and every cache/UI/action
  boundary, sharing remaining budget; remove canceled Handler tasks.
- Stop/cancel/drain/detach before VM destruction, preserving fatal/nonzero
  failure if safe drain cannot finish. Never detach a live callback or fake
  successful shutdown.
- Real fixture-owned HWND subclass must invoke actual native wrappers on its
  owner STA under the actual WindowManagerGlobal.mLock, preserve the real
  window join, require bounded unavailability before lock release, then prove
  drain/no late mutation, saturation and lifecycle controls/fresh process0.
- Preserve all14 positive Web3 cases, original3s/400s/60+60s and actual fresh
  app/device completion, then coherent full/package/e2e gates.

Early web3-query-compile builds actual objects with existing production Werror
flags in CI only. It supplies compile evidence, never runtime/fix acceptance.
No local C/C++, Java or C# compiler or executable fixture was run.

## Second WIP source slice (runtime unverified)

The four real native wrappers now hand off before ScopedEnv/attach/JNI and
publish deep native results only after worker detachment. Providers and their
children retain one HWND generation; retirement runs before Window's real
unmodified owner-thread join. Java receives native request tokens, rechecks
after actual global/window monitors, shares the remaining deadline with UI
Handler work, removes canceled tasks and guards cache/action publication.
Main and app-process exit paths stop admission/drain native work and event
callbacks before VM exit; failed drain takes explicit halt(1), not unsafe VM
teardown. This is source only: Java/C++ compile and real contention/lifecycle
regression are still required. The early 53c object compilation40570599325
passed; it does not compile or qualify this later source.

## Real fixture source checkpoint (still runtime unverified)

Test-hook build now dispatches all four existing query wrappers on a real
fixture HWND owner STA under actual WindowManagerGlobal.mLock contention.
The bare-window fixture requires an actual BLOCKED/viewRoots stack, the real
window close/join before releasing that monitor, the original3s query plus
500ms scheduling allowance (no deadline change), actual detach success and
no late root publication, then shutdown begun with one canceled JNI worker
still blocked. Its unwrapped real-JNI control must actually remain stuck and
is explicitly an expected failing control, not historical-binary evidence.

The mapped Dialog Activity tests two real window lifecycles per process:
positive button action, identical action queued behind held actual UI thread,
original-deadline unavailability, Handler removal/drained JNI, no late action
or cache insertion, then real Dialog dismissal and stale-generation rejection.
Both Main and device runs and every original14 Web3 case remain required.
Finite Windows driver source exists; exact coherent component producer/source
workflow and peer review/CI execution are still pending. No runtime acceptance.

Early compiler40574506577/source61883e31 uses the existing framework compiler
and hook-enabled native objects. Prior3350/862 startup failures were missing
caller actions-read permission, corrected before this run; no compiler or
fixture executed in those startup-failed runs. Any later source needs its own
compile/runtime validation.

## Component candidate prepared, not yet dispatched

The exact current component workflow builds fresh framework/app/APK/fixture,
NDK Web3 libraries and API3 translator plus current host/launcher/boot jars.
It pins immutable green AOT450/full40557661255 only for scaffolding, validates
all8 old suppliers/artifact identities and all5 fresh producers/4 packages,
hashes every current native host package member and all5 actual boot jars,
and rebuilds/copy-verifies all15 image files when any jar differs. Current
Java/fixture/artifact presence and producer-to-consumer source identities are
explicit. It runs the negative real-JNI control, actual4wrapper global-lock
regression, original14 Web3 cases, mapped action controls in Main/device and
strict fresh device shutdown; all original positive deadlines remain.

Standalone production pool core was independently qualified by Native at
25e708d2/run40575187153: actual128controls/host,0failures,bothWindows, explicit
11systemDLLallowlist/Werror and true21worker exit+handle-close probes/host.
Imported exact fixture/workflow commits; this is native-core-only proof.

Early618 compiler native objects and actual framework/app Java succeeded;
fixture compiler then failed because its sourcepath recursively compiled
Main's generated-service dependencies. The fixture now reflects actual boot
helpers from the newly built jar and compiles only its own source against
framework classes. No runtime fixture executed in that failed compiler run.

Native peer corrected the initial mapped lookup concern (PerformImpl has no
find call); explicit actualaction-phase counter nevertheless now proves the
queued task is the action, not a lookup. Sequential live HWND hooks are restored
after each joined sender, with payload identity checks protecting HWND reuse.
All of these later changes still need current CI compilation/runtime evidence.
