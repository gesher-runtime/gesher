# PR 399 review and local validation

Date: 2026-11-24. Reviewer and repair account: `palmoni5`.
Original PR head: `5bffcb1e0c57056afdeb706066bb541ccf6b0c01`.
Initial-main integration: `e074b67b2f83ce9128c11713171329e9fbbf6739`, merged
without conflicts as `73f0eb2ff25e9c73e8bfae733155ef5db06ea69a`.
Latest-main integration: `5b2af6e4e5de5525791c304de21c2c6a77aebcd2`, merged
without conflicts as `16e474e250a90942fcef517c3ddf1c79ed275a14` after clock
correction `3b3d4624f86ee4731800f592383ec2ee4a06eddc`. The upstream update
does not change ATM, PM or CallerIdentity; its ApkPackage and shared-library
changes are included in the fresh complete app build used for final Java QA.

## Scope and repair decision

The original service-worker feature advertisement was a merge blocker.
`SupportLib` advertised six `SERVICE_WORKER_*` capabilities, while the new
Java interception/settings helper had no production caller from either the
WebView provider or the native WebView2 host. Its direct helper tests did not
demonstrate real worker interception, cache policy or resource access. A caller
could therefore receive a positive feature answer and install a client that
ordinary worker requests never reached.

The required native request source is independently documented by Microsoft's
[`ICoreWebView2_22`](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2_22):
worker requests require the request-source-kind filter. Implementing Android's
worker settings also requires actual WebView2 network/cache behavior, not the
helper's `java.net.ResponseCache` and invented HTTP error responses. This PR
does not attempt to complete that bridge. Its WebView files are restored to
current main, removing all newly claimed worker capabilities and the unreachable
implementation. Existing WebView behavior is outside this delta. Neither W07
nor the complete W08 roadmap is claimed finished.

The retained work implements actual surface presentation and real, permission
checked usage observation. It does not substitute content or invent a device
capability. The 60 Hz pacing fallback already existed; real monitor refresh
replaces it when Windows supplies a mode. The revised scope is compliant,
subject to independent review and the exact published revision's required CI.

Focused graphics and usage repairs were reused from the already reviewed
`palmoni5/round-2-perf-compat-completion` branch at
`a36320ccd9e0bd8b983c9912ea95dc77d83536c9` (PR 391), then reviewed and tested
again here. The cumulative media/Bluetooth/memfd/storage changes from that
branch are not imported. PR 391 is not changed or closed by this work.

## Correctness findings and resulting behavior

- **Visible usage and attribution:** The original engine stopped counting at
  `PAUSED`, contrary to
  [Android 16 UsageStatsService's lifecycle path](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/usage/java/com/android/server/usage/UsageStatsService.java).
  A fresh original-source reproduction lost its only timer after a resumed
  activity became paused. The original 54-check suite nevertheless passed.
  Observation now starts with the first resumed or paused event, retains paused
  visibility until stopped/destroyed, captures the task-root package, and
  accepts authenticated reported feature-token usage.
- **Suspend accounting:** Independent review caught an incorrect clock change
  in the initial repair (`b3ee32c5`): it had used an older AOSP uptime contract.
  The runtime's pinned
  [Android 16 r4 AppTimeLimitController](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/services/usage/java/com/android/server/usage/AppTimeLimitController.java)
  uses `SystemClock.elapsedRealtime()` for registration, start/stop and timeout
  accounting. The service therefore retains its injected elapsed clock, as in
  PR 391. The exact source is Git blob
  `1b34b817def139c47734e1799dd52ee0646b40d6`, SHA-256
  `06fae6bdf1af1da03dc01bf6d273e40c4174f746505e06114eca0b01522af80c`.
  The corrected regression simulates 30 seconds awake and one day suspended
  while the activity remains visible. At the next STOPPED event and callback
  opportunity it delivers once with 86,430,000 ms of usage. The same corrected
  test fails against `b3ee32c5`; it does not claim callbacks run during suspend.
- **Ownership and concurrency:** Registrations authenticate caller identity,
  permission and installed-package generation. Admission preserves ATM-to-PM
  lock ordering. Retirement, replacement and unregistration revoke callbacks
  until their delivery starts. Callback delivery clears Binder identity and
  does not hold observer, package-manager or task-manager monitors. The suite
  exercises the actual service and complete lifecycle hook with declared
  Android host boundaries.
- **Retained damage:** Partial conversion requires a valid retained source and
  matching window-buffer generation. Resize, inset changes, cancelled buffers
  and intervening writers force full restoration. CPU locking uses top-left
  damage; queued non-CPU damage follows
  [AOSP Surface's bottom-left convention](https://android.googlesource.com/platform/frameworks/native/+/716acde0fa2cd40a587ac88657c1ab48697e4a48/libs/gui/Surface.cpp).
  Empty/invalid hints fall back to a complete frame. Layered windows coalesce
  damage and retain a complete DIB, using the real
  [`UPDATELAYEREDWINDOWINFO.prcDirty`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-updatelayeredwindowinfo)
  boundary. Failed publication, dim changes and cache resets require a full
  update.
- **Display pacing:** The refresh query runs in its own Windows translation
  unit so GDI SDK declarations remain available with production `NOGDI` flags.
  It observes monitor changes immediately and caches unchanged monitor-mode
  queries for one second. Queue surfaces have no consumer-display association;
  they use this process's foreground window or the helper/default monitor.

## Fresh local verification

| Validation on the repaired source | Result |
| --- | --- |
| Observer engine | 1,117 checks; one timer for 1,000 observers and 10,000 repeated events; no idle wakeups |
| Complete service and task-root lifecycle hook | Identity, permission, retirement, callbacks, lock ordering, visibility and suspend pass |
| Actual-framework activity CI entry, all groups | 901,542 checks pass against freshly built app classes |
| Usage query model | 12,655 checks pass |
| Complete unchanged app build after latest-main merge | Java and generated AIDL compile; BootLinkCheck 296 references / 0 unresolved; D8 passes |
| Surface geometry / release / damage, ASan and UBSan | 158 / 38 / 75 checks pass |
| Layered bitmap cache, ASan and UBSan | 1,441 checks pass |
| EGL readback and display refresh, ASan and UBSan | 12,000 full-frame comparisons / 44 checks pass |
| Frame recycling, ASan and UBSan | 315,844 checks pass; no steady-state payload allocations |
| Video scaling, ASan and UBSan | 32,165 checks pass |
| Display refresh real Win32 SDK compile/link | x64 and ARM64 pass with production flags, including `NOGDI` |
| Complete `gesher_window.cc` and `gesher_popup.cc` syntax compilation | x64 and ARM64 pass with production definitions and `-Wall -Wextra -Werror` |
| Whitespace / scope audit | `git diff --check` passes; no WebView, Services or PermissionCatalog delta against current main |

The graphics measurement used the same exact-source fixture in candidate/main/
main/candidate order, with 400 1080p frames per pass. The 64x64 damage case took
3.728 / 2.513 ms in the candidate passes and 1,291.948 / 1,051.533 ms in the main
passes; checksums matched. Full-frame timings were 1,149.228 / 1,134.152 ms for
the candidate and 1,343.480 / 1,154.732 ms for main. These are host CPU copy
measurements, not compositor/frame-rate results or a reliable full-frame
speedup claim.

The clock correction's regression passes on `16e474e2` and fails against
`b3ee32c5` at the elapsed suspend assertion. A separate reviewer independently
constructed the same pinned-contract expectation outside the repository and
confirmed that negative/positive result. The production and test sources at
`16e474e2` include the latest-main merge; subsequent updates to this record and
the usage-contract research document only record validation evidence.

## Validation limits

The host fixtures explicitly replace Android native identity, transport and
Win32/GDI boundaries. Real framework classes and full native SDK compilation
reduce API/build uncertainty, but do not establish Windows compositor timing,
ART/Parcel transport or real hardware behavior. The local framework/core inputs
are prebuilt. The complete AOSP sources needed for SOURCE-retention compatibility
metadata are absent, so the app build is a validation artifact rather than a
distribution build. LeakSanitizer cannot initialize under this container's
tracing; `ASAN_OPTIONS=detect_leaks=0` was used locally while ASan, UBSan and
explicit lifetime checks remained active. CI settings are unchanged.

Required Windows CI and merge authorization belong to the exact published
revision. This record does not claim those later actions have occurred.
