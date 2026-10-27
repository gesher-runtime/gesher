# Web3 native query boundary source checkpoint

Status: incomplete source, not a correction/runtime acceptance claim.
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
