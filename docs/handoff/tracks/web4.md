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
complete diagnostic is 40432795403 at 2755f21, including the coordinator's
verified startup profile merge2ba9a11. Both original boot gates now pass.
Windows 2022 has all14Web3/zero failed tests; 2025 fails device A11y teardown
after all UIA/app assertions complete, then two subsequent device NetActivity
launches. Package and both required e2e jobs pass. Web3's next source adds
teardown/tail diagnostics without changing deadlines or oracles. Verified
Web3 must merge before dependent Web4 implementation in its separate worktree.
No contract above is closed by scoped passing checks or this handoff correction.
