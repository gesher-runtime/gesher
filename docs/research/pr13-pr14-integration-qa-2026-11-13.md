# PR 13 / PR 14 integration and independent merge QA

This records the independent source review and local verification before the
two compatibility PRs are merged. Final published-candidate Windows evidence
is recorded in [PR 14](https://github.com/gesher-runtime/gesher/pull/14).
Local host models do not substitute for that final backend gate.

## Scope and identities

| Input | Identity |
| --- | --- |
| Original common main | `6bfc980c335a8b8ae1a9271dc5da43078561b516` |
| PR 13 reviewed head, 52 changed files | `45c1c1d8003d81905cc674a4224f3953dbd7f51f` |
| PR 14 isolated reviewed head, 60 changed files | `1b6827d2369eb9b6a46381e803940b7b11c85556` |
| Automatic union before repairs | tree `93ac1a087ec6ae800a10efd110e50cefc26b14e6` |
| Reviewed combined Java | tree `86039f71f9fe7dc21a2703391bfa15e6a8e8c41a` |
| PR 13 merge into main | `b9a94530c6bf91f7a14913e3ef284dd76bdf28bf` |

Three reviewers independent of the integration repair examined the complete
original changed-file sets, surrounding callers and combined interactions:

- **Java/application:** all 15 production classes and associated fixtures,
  drivers and provenance: Wi-Fi values/proxies; addresses, links and routes;
  capabilities, requests and specifiers; callback identity/retirement; and
  WebMessagePort/WebViewProvider ownership. Pinned Android 16 imports were
  independently retrieved and byte-checked with documented adaptations.
- **Media/graphics:** AudioTrack ring, resampler and state; SoundPool ownership;
  both surface implementations; Codec JNI receivers, workers and waits;
  InputSurface admission and Codec/Recorder consumers. All associated tests,
  declared host seams, CMake targets and Windows wiring were reviewed.
- **Native/CI/documentation:** complete epoll, vector, select, timer/registry,
  bionic and ARM64 changes; native and DNS fixtures; workflow, CMake and Windows
  wiring; preservation policies; both inventories and provenance limits. This
  reviewer independently inspected and tested both integration repairs below.

All three approved PR 13 before its merge, following successful exact-head
[full CI](https://github.com/gesher-runtime/gesher/actions/runs/43349331608)
and [ARM64 CI](https://github.com/gesher-runtime/gesher/actions/runs/43349331552).
Its actual merge tree is `468dd2815dc6dea57de336866be54ecb65350d6d`, identical
to the tested tree. PR 14 retains its own fresh combined CI gate. Both Windows
tracks 96 and 97 remain enabled. Eight originally shared files merge without
textual conflicts; this alone did not establish semantic compatibility.

## Timerfd and empty vector writes

PR 13's EventObjectWriteError accepted every eventfd-backed object. PR 14's timer
readiness uses a read-only EventFd. Empty/all-zero writev therefore returned
before scalar WriteEventFd could reject it. Windows reserves the NUL descriptor
with GENERIC_READ | GENERIC_WRITE and _O_RDWR, so its native granted access
cannot enforce the logical timer restriction.

The helper preserves inotify EBADF and accepts only an eventfd whose immutable
read_only flag is false. Timers and epoll return EINVAL before the shortcut.
Ordinary eventfd/file/pipe/socket behavior remains covered. The flag is set
before publication; this adds no mutable-state or lock-order dependency.

The production-routing fixture checks empty, one-zero, all-zero and payload
vectors on an original timer and a duplicate surviving original close, plus
absence of injected readiness. Its host adapter retains the underlying kernel
description independently of the first public descriptor. The unchanged
combined helper compiles and fails exactly six new empty-vector assertions;
the fixed source passes all 64 vector assertions.

The shared public fixture adds twelve assertions across original, surviving dup
and dup2 descriptors. The Linux reference, actual Windows timer executable and
both Android guest ABIs include them. The zero-count public call uses a valid
vector pointer to satisfy Bionic's nonnull annotation while testing count zero.

## Windows ARM64 fixture coverage

Independent review found that the Windows typed timer adapter uses its own
fixed dispatch numbers, but the included fixture asserted the Linux ARM64
syscall table whenever __aarch64__ was defined. Actual LLVM-MinGW ARM64
compilation of the old whole translation unit fails precisely that assertion.

The table assertions now apply only to real Linux/Android guests; the Windows
typed adapter is unchanged. The ARM64 workflow explicitly builds
timerfd_windows_test and verifies its PE/COFF architecture. Its previous
limited target list omitted this new default CMake target.

Independent verification with LLVM-MinGW 20261006 and Android NDK r27c:

- Fixed complete Windows timer fixture compiles with -Wall -Wextra -Werror;
  the resulting object is coff-arm64/aarch64.
- Both real Android ARM64 and x86-64 timer fixtures compile with warnings as
  errors, including the twelve new vector assertions.
- Both real guest syscall-table assertions remain active: correct tables
  compile and deliberately wrong constants fail on each architecture.

This establishes build coverage, not native ARM64 hardware execution.

## Fresh local verification

| Area | Observed result |
| --- | --- |
| Java callers | 599 assertions: Wi-Fi 133; snapshots 175; builders 120 + 65; callbacks 28; WebMessage 28 + 21 + 29 |
| Combined app and complete services | Fresh javac/D8 builds pass |
| Public AudioTrack/Codec fixtures | SDK36-only compilation passes |
| Media/graphics models | 754 ASan/UBSan assertions: audio 162; SoundPool 10; geometry 158; Codec 16 + 36; callbacks 55; timestamps 20; window ownership 31; existing media APIs 266 |
| Combined event-loop models | 107 ASan/UBSan assertions: epoll 15; vector 64; select 28 |
| Timer model | 77 ASan/UBSan assertions |
| Public Linux timer reference | 68 assertions after the vector additions |
| Original PR 13 public Linux event-loop reference | 66 assertions |
| DNS ownership/caller | 103 + 85 ASan/UBSan assertions; both close-omission controls fail their required ownership checks |
| Policies/parsers | Four vector rejection controls and 31 inventory/service parser tests pass |
| Workflow syntax | Changed YAML parses; 71 applicable main/framework Bash blocks pass syntax checks |

Old media controls compile and fail the intended three receiver, seven waiter
and eight callback assertions without unsafe accesses to retired objects.
Independent pinned Android source checks cover audio, SoundPool, surfaces and
Codec callbacks. The pinned MediaCodec Java callback enqueues Handler messages,
avoiding inline application release on the native worker.

The executor explicitly rejects LeakSanitizer under its ptrace environment.
Only local reruns use ASAN_OPTIONS=detect_leaks=0; ASan/UBSan remain enabled and
repository CI sanitizer settings are unchanged.

## Provenance and final acceptance

SDK36 SHA-256 `d9eb9da824d9e247a352f570f01e1169e725b2954bca9e283a71786c59b59f9a`
and R8 SHA-256 `3502f087388ce95bbf5254555927f09385dc2636873964d0bd53f3eaf4fb49ad`
match workflow pins. The compile framework archive SHA-256 is
`85f895adba935980f7255fadc182cb413c8ffec13ee1b72b80622c583c830736`, independently
verified against artifact `14009304784` from isolated PR 14 run `43363980751`.
Production app classes were freshly built from the combined sources.

The existing inventories retain their original isolated measured snapshots.
Complete-list sizes, totals, source trees and generator hashes were checked;
80 PR 13 and 103 PR 14 locally available source receipts match. A historical
external AndroidRuntime.cpp input was unavailable and was not presented as
freshly reverified. Neither historical JAR hashes nor isolated API counts are
relabeled as a fresh combined inventory.

The final candidate must pass the full workflow, Windows 2022/2025 runtime,
framework and E2E gates, and the extended ARM64 build before PR 14 is merged.
Its merge tree must match the tested candidate. This review does not establish
universal APK compatibility, complete Android API coverage, physical device
qualification or native ARM64 runtime execution.
