# Native stability candidate QA, 2026-11-05

The resumed candidate builds and passes the focused native regressions below.
The installed Gesher runtime and shared apps were left running; all managed
fixtures used private workspace data and no shared Binder endpoint.

## Build and source identity

CMake/Ninja completed the ten targets `dalvikvm`, `gesher-exec`, `dex2oat`,
`binderd`, `servicemanager`, `socket_probe`, `epoll_probe`, `elf_load_test`,
`bionic_key_exit_test` and `arm64_elf_test`. The final native implementation is
commit `0aafccec6d7c2cfdd88d73e721a3e1ce523edf2b`; its native source/test paths
were clean at evidence capture. The host was relinked after the epoll fix and
all checks below were rerun. Its runtime hash is
`eb5033dad481b248f53f5b28f6d16feace695d20064a89e88e8ce35a4adca095`.

The original `out/native-build/logs/build-input-manifest.json` describes the
earlier build and must not be used as the final source identity. The resumed
receipt hashes 711 current source/test files, the tested binaries and fixtures,
and the read-only boot JARs. All 123 AOSP dependency HEADs were rechecked against
the original dependency receipt. The host uses llvm-mingw 20261006. Local NDK
fixtures use 28.2.13676358; CI's independently pinned NDK is 27.2.12479018.

## Executed checks

| Check | Passing result |
| --- | --- |
| `socket_probe`, `epoll_probe`, `bionic_key_exit_test` | Three processes exit 0; epoll retains both sets of 200 pipe round trips. |
| Fresh `libc_test.so`, x86_64 | 100 library checks; loader's own checks also pass. |
| Fresh `libc_test.so`, translated ARM64 | 104 library checks; ARM64 loader's own checks also pass. |
| Matching ARM64 translator smoke | Exit 0, including four-thread LL/SC increments. |
| `FrameworkMemoryTest`, `FrameworkScryptTest`, `FrameworkLocalSocketTest`, `FrameworkCharsetTest` | Four private ART processes exit 0 with their success markers. |
| Binder Debug object counters | Private ART process exits 0 after observing local object creation and Parcel release. |
| Fatal-context ELF and host fixtures | Both intentionally exit 1 at fault address 0x308, with RIP/RSP and no startup error; host also reports DLL+offset. |
| Full `native2`, x86_64 and translated ARM64 | Three consecutive cycles per ABI, six private ART processes, all exit 0. ARM64 FPCR/FPSR and 256 environment rows also pass. |

The fresh libc fixtures include search-tree callback ownership, pthread cleanup
ordering, the four-cycle eventfd/TCP exchange, nested epoll readiness, graph
cycles/depth, concurrent graph reversals and both blocked epoll waiters.
Their compilation retains the three existing libc-test warnings; the rebuilt
native2 fixtures compile with `-Wall -Werror`.

The checked-in Windows tracks and `tests/windows/run-framework-jni.ps1` retain
these regression entry points. Local build/run helpers and full logs remain
under `out/native-build`; synthetic fatal logs remain under
`out/telegram-network`.

## Deterministic epoll lost-wakeup fix

Final review found that registering only the generic poll wake channel left
a race after the epoll interest snapshot. A change in that gap could be lost
if another waiter consumed the shared auto-reset interrupt first. The new
probe pauses exactly at the gap, publishes an already-ready interest, runs
another waiter and resumes the paused one. With the previous implementation
it fails by timeout; with private channels it passes.

Each waiter now registers its own epoll interrupt before taking any snapshot.
Updates and removals signal all channels under the same epoll mutex. The
waiter retains the epoll object across concurrent descriptor close, removes
its channel under that mutex and closes the handle afterwards. A paused-wait
close/replacement control passes, and 128 zero-timeout returns leave the
process handle count unchanged. Logs are
`out/native-build/logs/epoll-snapshot-{before,after}.log`.

## CPU-clock test correction

The first full ARM64 native2 run failed only its "idle caller" CPU comparison:
the target and caller each accrued 109.375 ms. An unchanged rerun passed.
The old caller repeatedly ran a translated phase-polling loop with 1 ms sleeps,
so this did not establish a production CPU-clock defect.

The fixture now blocks on a condition variable with one 15-second monotonic
deadline, while the target remains active and signals completion. The target
checks cancellation before publishing completion. The strict checks that the
target accumulates at least 50 ms and exceeds the caller's CPU delta remain.
All six rebuilt runs report target 109.375 ms and caller 0 ms. No production
clock implementation was changed. The first failure is preserved alongside the
passing runs.

## Exact local evidence and limits

`out/native-build/logs/resumed-native-source-manifest.json` SHA-256:
`ee00cd4080b702a43894d4faa676c31cc80ef8d1af454dbe54ab7f793c1345a5`.

`out/native-build/logs/resumed-native-qa-receipt.json` SHA-256:
`562c83ef362f96921e3e6dc2e5c392dbf0849670a44c94583a44792918037cb7`.

Earlier pre-epoll receipts are retained with the suffix
`-before-epoll-fix.json`; they do not describe the final executable.

These are local regression results, not fresh installed-app or end-to-end
packaging acceptance. ARM64 here means translated native ELF libraries on
the x86_64 managed host. The existing ashmem mapping-protection limits,
unsupported local-socket namespaces/SCM_RIGHTS, epoll edge-trigger/alias
lifetime limits, and unavailable native unwinding remain documented in the
individual reports. Telegram's original native crash remains unexplained;
the fatal-context patch improves its next report rather than fixing it.
