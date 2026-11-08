# Local JavaScript bridge/cookie regression

Build with the usual cached Android build tools and JDK:

```sh
bash tests/apps/bridgecookies/build.sh out/app --framework-res <framework-res.apk> --framework-classes <framework-classes.zip> --core-classes <coremod>
```

Run all four activities through `powershell -NoProfile -File tests/windows/run-webview-login.ps1 -Dir <runtime> -Apk <bridgecookies.apk>`. The runner creates a unique empty Android/browser profile for each case, uses hidden test windows, requires success markers with no failure lines, shuts down only each isolated device, and retains evidence. `-SkipWorkers` explicitly skips the worker activity for development diagnosis. Fresh per-case profiles also avoid depending on an existing WebView2 browser process when the previous app process exits.

Profiles use short unique paths under the checkout's `out/wv-p`; the evidence directory records their root in `profile-root.txt`. This keeps Chromium's nested cache paths below legacy Windows limits without changing browser security settings. Deeply nesting a profile under evidence paths caused controller creation to fail before test pages loaded during development; those failures are retained separately.

The runner supplies fresh Android/browser data for every activity; an activity's later native configure call does not override an already configured environment. Each activity starts an ephemeral HTTP server bound only to `127.0.0.1`. It uses fixed, nonpersonal fixture values and never opens Google or another external site. The server discards request bodies without logging them. The test verifies synchronous ordered void/nonvoid calls on a Java worker outside both the Android UI and WebView2 STA, real HttpOnly cookies read by `CookieManager` from that callback, Java exception recovery, large input/output, invalid/stale/destroyed reserved transport denial, and zero POST/body/bridge traffic reaching the origin.

The current transport keeps Java arguments in native memory, registered through the synchronous host object. HTTP bodies carry only opaque one-use nonces. The cookie test observes those bodies and rejects JSON/private markers, while Java still verifies the original arguments; consumed and cancelled nonces must return 403. Every worker case prepares its own fresh valid nonce, so a shared/service-worker rejection cannot pass merely because its nonce was already consumed. The server also counts a fixed private fixture marker directly from body chunks without retaining their contents.

The destroyed-view case retries that view's previously valid request against a replacement view. `InflightDestroyActivity` also destroys the view from the UI while a real bridge callback is waiting on its Java worker, allows the late callback to finish, then checks that a replacement view rejects the old endpoint without reentry or HTTP origin traffic. A separate native helper test covers cancellation and timeout completion counters. These are focused regressions, not an exhaustive teardown-race stress test.

`NavigationActivity` checks fragment changes, hash-only history calls and path/query history calls against Android's completion-callback rules. `WorkerActivity` uses real dedicated, shared and service workers: delegated dedicated-worker requests reach Java exactly once; shared/service worker requests are rejected without Java invocation or origin traffic.

`-OnlyCase <case-name>` runs one explicitly named case for diagnosis and records that selection; it is not a full-suite pass.
