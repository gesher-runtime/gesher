# Windows app stability: integrated candidate QA, 2026-11-05

This report covers the isolated `codex/stability-apps-20261105` candidate
relative to `5f6b3444699de6c4a88e239de0ef67c6a9bbfbee` (`origin/main`).
The unrelated microG/account work in the main checkout is excluded from the
resulting diff. Installed runtime files and existing app/device processes were
not replaced, stopped or restarted during this continuation. Tests used private
ART processes and data directories.

## Changes verified

- Native startup: search-tree functions and pthread cleanup records; real
  Binder object counters; useful Windows fatal-register reports.
- JNI: MemoryFile, MemoryIntArray, Scrypt, filesystem LocalSocket streams and
  CharsetUtils, including checked error paths and actual framework callers.
- Readiness: nested epoll, cycle/depth rejection, nonconsuming poll readiness,
  ONESHOT and a private topology channel registered before each waiter's first
  snapshot. Other waiters cannot consume that channel's pending wakeup.
- Services: desktop device-idle policy, valid unsupported thermal results,
  selected usage/shortcut/unmanaged-policy contracts and in-process service
  registration callbacks. Callback dispatch invokes observers outside locks,
  including concurrent observers that publish each other's service, and rejects
  stale publications before they can overwrite a pending current service.
- Java values: pinned Bluetooth/BLE, connectivity/request/parcel and MediaStore
  client APIs. MediaStore insertion tolerates an absent provider, failed cleanup
  and null input streams; decoding closes streams on exceptions.
- Notification-settings intents use the fixed Windows notification-settings
  URI when no Android handler exists. Browser behavior remains covered.

The underlying diagnoses and retained backend limits are documented in the
[app report](app-stability-2026-11-05.md),
[JNI report](framework-jni-contracts-2026-11-05.md),
[native QA report](native-stability-qa-2026-11-05.md),
[epoll report](epoll-nesting-2026-11-05.md),
[MediaStore report](mediastore-contracts-2026-11-05.md) and
[network value source notes](../../app/java/android/net/VALUE-SOURCES.md).

## Integrated Java and source checks

| Check | Observed result |
| --- | --- |
| Full `app/build.sh` candidate and `tests/dex-services/build.sh` | Compile and D8 succeed using the recorded framework/core class inputs. |
| Full candidate app + rebuilt host, four value fixtures | 127 assertions pass: network 30, network requests 38, Bluetooth 31, MediaStore 28. No value overlay is needed. |
| Desktop service fixtures | DeviceIdleContractTest, SoftwareServiceContractTest, ServiceRegistrationCallbackTest and ManagerConstructionBatchTest pass, including construction of 17 real ContextImpl managers. |
| Existing app regressions on full candidate | FrameworkFixesTest passes its 14 families; NotificationSettingsIntentTest and BrowserIntentTest pass with mocked Windows openers. |
| MediaStore negative control | The prior implementation with the same resolver fixture fails in `delete(null)`; the fixed implementation passes. |
| Inventory/parser/font unit tests | 63 pass: compatibility 18, API inventories 9, member resolver 22, JNI source evidence 3, font verifier 11. |
| PowerShell and diff checks | Changed scripts parse; `git diff --check` passes. |

The four value fixtures are registered in bounded Windows track 94; the service
fixtures are in track 50. Framework JNI and fatal-context fixtures also remain
part of their normal test routes. New JNI source-inventory tests run in CI.

The source inventory recognizes the four reviewed `register_gesher_*` aliases;
it no longer reports their upstream registration names as missing local
implementations. Source references remain unverified behavior in that inventory.

## Native verification

Ten native targets build with llvm-mingw 20261006 and 123 AOSP dependency
revisions were rechecked. Fresh libc fixtures pass 100 x86_64 and 104 translated
ARM64 checks, including search ownership, pthread cleanup, the loopback network
exchange and nested epoll. Three standalone probes, the matching ARM64 smoke
test, four framework JNI fixtures and a real Binder-counter fixture also pass.
Both disposable fatal fixtures report the expected SIGSEGV, RIP/RSP and host
module offset where applicable.

The final epoll probe forces the gap between taking an interest snapshot and
entering the poller. Another waiter consumes the old implementation's shared
signal: that implementation times out, while the private-channel version
reports the ready descriptor. A retained-object close/reuse test and 128
zero-timeout calls with an unchanged Windows handle count also pass.

The full native2 suite passes three consecutive cycles per ABI. Its initial
ARM64 CPU-clock comparison failed once and passed unchanged on retry. The
fixture now blocks its caller on a condition variable with a 15-second
monotonic deadline, keeping the strict independent target-CPU assertion. All
six final runs pass; no production clock implementation changed. The first
failure and the exact 711-file native-source receipt are retained in the
[native QA evidence](native-stability-qa-2026-11-05.md).

## API presence measurement

The final full app was compared with the same five installed boot JARs and
Android API 36 reference (`android.jar` SHA-256
`d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`).
Of 90,630 SDK member declarations, missing declarations inside present classes
decrease from 353 installed to 215 candidate. Declarations whose owning class
is absent decrease from 8,693 to 7,958. No present-to-absent transition was
recorded. The report still records 500 possible flag mismatches separately.

These are descriptor/presence results, not semantic defect counts or an Android
support percentage. Optional modules, final/visibility flags, constant values,
reflection, hardware and IPC interoperability require separate assessment.
The earlier central inventory remains a historical snapshot; use this report
for the final candidate's measurements.

## Input identities and local evidence

| Input | SHA-256 |
| --- | --- |
| Rebuilt `dalvikvm.exe` | `eb5033dad481b248f53f5b28f6d16feace695d20064a89e88e8ce35a4adca095` |
| Full candidate `gesher-app.jar` | `0c74e495f8c272a857ffc21812c9cc5a7b2d57a38abe550c718e21602c292c24` |
| Final `services-tests.dex` | `fda8b5f853803cde770c86b338422b63d8505be6dc4f364f03b8c1d70ccb4248` |

Detailed evidence stays in ignored workspace output, rather than uploading
application logs: `out/stability-worktree/out/resume-qa/` contains the tool,
service/JNI source and final member inventories; `out/java-review/` and
`out/service-contracts/` within that worktree contain source/input receipts
and bounded process results. The member inventory hashes every boot input.
The final full-app service receipt has SHA-256
`286c0bbb502de7e477d7038f5dbac6b1d25a2794cb759fd712e8eeff1c3a77a7`;
all 229 compiled source hashes were rechecked against the final worktree.

## Remaining scope

This verifies the listed candidate contracts. It is not a newly installed or
fully packaged release, complete Android conformance, or a full physical-device
test. Bluetooth hardware, abstract local sockets/ancillary descriptor transport,
complete ashmem protection, MediaProvider/media playback, all service methods,
general cross-process lifecycle and older-process parcel interoperability remain
separate work. The later Telegram SIGSEGV remains unexplained; fatal-context
improvements help diagnose its next occurrence and do not claim to fix it.
