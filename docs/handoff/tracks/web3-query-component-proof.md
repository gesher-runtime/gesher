# Web3 bounded native query correction: verified component

Exact sourcefc17670355e8a694ecc83b632eb5e2610a968796, run40577659058 attempt1.
Metadata9jobs:8success,1intentional translator smoke skip. All six producer
logs and both actual Windows logs were read; integration independently reread
them. This is component acceptance, not canonical full/default-OFF/package/e2e.

## Actual deterministic evidence on both Windows hosts

- Unwrapped real JNI on the actual window owner enters viewRoots under the
  main thread's real WindowManagerGlobal.mLock. Its real window join remains
  stuck after15s and the owned negative-control process is terminated. This
  is an expected failing control, not execution of the historical binary.
- Corrected existing four wrappers each reach actual BLOCKED/viewRoots under
  that monitor, return unavailable and finish the REAL native window close
  and owner join BEFORE the main thread releases the monitor. Measured total
  query+join3010-3038ms(2022),3010-3021ms(2025), original native3s budget plus
  at most500ms scheduling observation allowance; no production deadline moved.
- After release, actual JNI_OK detachment/active0/cache0, sender exit0 and
  repeated HWND generations pass. Shutdown starts with one canceled worker
  still attached and blocked; release then drains/detaches and the process
  exits0. Terminal shutdown refuses restart.
- Real mapped Dialog windows: in BOTH Main and device app processes, two
  positive button clicks prove the operation mutates a live View. Two identical
  ACTION-tagged Handler tasks queued behind the held UI thread time out at
  the original deadline and are removed. After UI release no late click/cache
  insertion occurs; JNI is detached. Real dismissal then rejects two old-
  generation actions without mutating the old View. All senders exit0.
- All original14 Web3 cases pass on each host, including actual Android/UIA
  button/edit/toggle/WebView operation, original400s/60+60s and normal app-child
  completion0. Native close enter/returned pairs are retained in raw logs.
  Both original and fresh mapped-device shutdown launchers actually exit0.
  Each host reports original-failures=0,total-failures=0.

## Actual source/package identity

Fresh current framework/app/APK/fixture, current NDK Web3 libraries, API3
translator and current host/launcher/compiler/three core jars are supplied.
Every8 immutable AOT450/full40557661255 scaffold job/artifact is pinned, as
are all5 current compiler producers/4 current artifact packages. Current
host members are SHA256-checked after copy, all5 actual boot jars are printed,
and fresh images are rebuilt with current compiler and copied with all15
art/oat/vdex SHA256 equality checks. No imageless or stale-jar fallback.
Current artifacts: framework12768097791; host12769089231;
NDK12768495900; translator12768287047; numeric reference12769045128.

Independent native pool source is byte-identical to qualified25e708d2:
run40575187153,128controls/host0, explicit systemDLL allowlist, actual worker
exit/handle-close/resource parity. That remains native-core-only evidence.

## Exact logs retained locally (plain logs only, no artifacts downloaded)

Prefix C:/work/gesher-ci-review/web3-query-component-40577659058-:
- test2022.log job122462084084; test2025.log job122462084240.
- host.log122448294249; hosttools.log122448294470;
  fpsr-reference.log122448294507; ndk.log122448294820;
  framework.log122448372755; translator.log122449427618.
- result.json and watch.log retain terminal identity/status.

Original failure40550620230/f4b, exact96b module/maps and WCT remain unchanged:
main destructor join, owner ART monitor wait, WebView STA proven; the exact
historical Java monitor/owner was not captured. This component demonstrates
and corrects the source-level inversion; it does not invent that missing fact.

## Remaining acceptance

Canonical changed-union full must run normal suite/package/e2e with production
GESHER_WEB3_QUERY_TESTS=OFF AND actually execute the new regression using a
separately named SAME-source/compiler hook runtime with explicit source/role
manifests, coherent jars/images and installer exclusion. Component used the
hook build and does not prove that later full or release packaging. Hardware
keyboard/IME and other physical positives remain open; no release claim.
