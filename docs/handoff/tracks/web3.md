# Track brief: web3

## Status and next steps

Accepted Windows4/Web3 full40466021634 and coherent Net440499454478 remain
valid bounded evidence. A later device-A11y teardown recurrence in AOT
full40512344714/96b still lacks a native stack. The next independent CI
diagnostic uses exact accepted694 ART/Java/image artifacts and a separate
bounded WCT/native-stack observer after the unchanged original failure.
[Source, provenance and retained limits](web3-teardown.md). No causal fix
or new full acceptance is claimed; compile/control/runtime proof is pending.

WebView and network leftovers reported by the webview2 and net tracks. The
original brief given to the first agent of this track (2026-10-26), kept
verbatim. A successor reads it together with the "Status and next steps"
section in the track's doc on its branch.

You are the "web3" track agent of the gesher project. Your worktree is
C:\work\gesher-webview (reused) on branch wip/web3 (cut from
phase4/integration and pushed). Work only there, with absolute paths under
C:\work\gesher-webview (another agent once wrote into the coordinator's
worktree by mistake: check `pwd` before editing).

FIRST read docs/handoff/track-rules.md and follow it exactly: it is binding.
Then read docs/webview.md and ADR 0050 (its limits), docs/network.md and ADR
0037 (its leftovers), docs/services.md (accessibility, if any) and
tests/windows/tracks/37-webview.ps1 and 56-net.ps1.

GOAL, each verified in CI:
- WebViewCompat / androidx.webkit: the boundary interfaces androidx.webkit
  looks up (WebViewProviderBoundaryInterface and friends:
  `WebViewFeature.isFeatureSupported`, WebMessageListener /
  addWebMessageListener, postWebMessage, document-start scripts,
  setSafeBrowsingEnabled, ProxyController, the features WebView2 can
  back), so apps built with androidx.webkit work; a test app with
  androidx.webkit from Maven (pinned by SHA).
- Web content in Android's accessibility tree: the WebView exposes its
  page's accessibility nodes (AccessibilityNodeProvider) from WebView2's
  UI Automation tree, as far as Gesher's accessibility support goes (check
  what exists; if Gesher has no AccessibilityManager service with clients,
  make it real: a Windows screen reader (Narrator) reads Android views
  through UI Automation, the reverse bridge, and record the extent in the
  ADR).
- Geolocation: WebView's GeolocationPermissions and the page's
  navigator.geolocation answered from Gesher's LocationManager (verify
  position values on the runners with a test location source).
- Network leftovers from ADR 0037 / docs/network.md: the system proxy
  (Windows' proxy settings and PAC as ConnectivityManager's default proxy,
  Proxy.getDefaultHost, and used by HttpURLConnection/OkHttp),
  CONNECTIVITY_ACTION broadcasts and the other leftovers that list names.
- Make the net track's external https check robust: it failed on
  windows-2025 (ci 40240210807 attempt 1, "missing: NetApp https: 200
  Example Domain") because it depends on example.com; serve the page from
  a local https server with a test CA on the runner (or retry with a
  clear reason), so CI does not depend on an outside site.
Tests in tests/windows/tracks/54-web3.ps1 (test APKs of your own), both
runners, single process and on the Gesher device; 37-webview and 56-net
must stay green; e2e too. Your file domain: the WebView provider and its
WebView2 host code, the accessibility service and its UIA bridge,
ConnectivityManager's proxy and broadcasts (net's files: careful edits),
LocationManager's WebView hookup, tests/windows/tracks/54-web3.ps1 and its
test apps (framework.yml: one line per test app), 56-net.ps1's https check,
docs/webview.md and docs/network.md sections, ADR
docs/decisions/0058-web3-webviewcompat-accessibility-proxy.md (0058 is
reserved for you), the roadmap boxes.

Coordinator notes: CI first; NEVER build AOSP locally. Other tracks run in
parallel on the shared CI (media, windows3, aot2, exec2, surfaces,
native2): trigger CI only when code is ready, cancel superseded runs. Watch
with `gh run watch <id> -R gesher-runtime/gesher --exit-status --interval
120`; on HTTP 403 back off ~10 minutes. Keep a "Status and next steps"
section updated with every meaningful push. Before your final CI run, merge
origin/phase4/integration into your branch. Final message = the report
track-rules.md asks for (run ids, PASS lines, remaining gaps, edits outside
your domain, the verified SHA).
