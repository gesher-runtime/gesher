# Track brief: web4

Remaining WebView contracts after web3; net4 owns network/socket/resolver
details separately. Reserved ADR: 0072. Branch `wip/web4` starts from current
integration; merge verified web3 before implementation in its own worktree.
Follow track-rules.md, CI-only AOSP builds, absolute paths and no WSL.

Read docs/webview.md, ADRs 0041/0050/0058, web3's final report/status,
GesherWebView's provider and AndroidX boundary implementations and the native
WebView2 bridge. Existing web3 proof does not close the limits below.

Required contracts:
- Listener/document-start script/geolocation behavior in child frames, with
  correct frame/origin identity and permissions. The current paths only
  establish main-frame behavior.
- Page-to-app transfer of MessagePorts, including ownership, ordering and
  callbacks; typed ArrayBuffer transport alone is not port-transfer proof.
- ProxyController changes after the WebView2 environment is already created,
  including required environment restart and remaining reverse-bypass behavior.
- Client certificate keys outside the Windows key store, SafeBrowsing
  callbacks and the remaining certificate/permission/error paths documented
  by the WebView status and ADRs.
- Remaining geometry/cutout/text zoom/capture/document-picker contracts and
  AndroidX advertised features. Test actual effects, not only feature lookup
  or accepted setters, and keep unsupported features explicit until closed.

Audit every still-open webview.md/ADR 0041/0050/0058 item against later CI
evidence before implementation. Keep the exact actual-root defects and
format/ownership/hardware limits; do not remove an item as historical without
source or executable evidence. Network binding/DNS/Wi-Fi/VPN/captive portal
and legacy resolver ABI follow-ups belong to net4, ADR 0070.

Use local origin/frame/TLS fixtures, both Windows/WebView2 versions,
single-process/device modes, actual UIA from another process and a real
WebView-dependent app in e2e. Keep origin checks, callback sequencing,
negative paths, pixels/input and native ABI evidence. Merge latest
integration before final full CI; read package cleanup and both e2e jobs.

## Status and next steps

Reserved on 2026-10-26. No successor implementation or CI. Web3's latest
complete diagnostic is 40376044459 at
939ec313d0fe5a2d8410ee700a927fad0538269e: all 14 Web3 checks pass on both
runners, package and both required e2e jobs pass, but Windows 2022's boot-profile
performance failure keeps the full stage red. The older geocoder/log-framing
failures above are superseded, not the current blocker. Web3 preparation now
includes verified runtime/exec3 integration 94ccde1; no repeat run is dispatched
while the coordinator diagnoses startup. Verified Web3 must merge before
dependent Web4 implementation in its separate worktree. No contract above is
closed by native-only Windows4 evidence or by this handoff correction.
