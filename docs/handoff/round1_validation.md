# Round 1 continuation: validation and remaining integration gates

Date: 2026-11-23. Continuation: [PR #388](https://github.com/gesher-runtime/gesher/pull/388),
authenticated account `palmoni5`. Original work: [PR #381](https://github.com/gesher-runtime/gesher/pull/381).

## Changes

The original four commits at `92854cf5c7cae7c696c1ce1a0ee7b1c47ce5cd9e` are
preserved. They include the PunchView dead-tail repair already requested in the
older handoff. Current main at `bcfb5044029d486e17138dc417f3ced81524cf3f` is merged.
The conflict in `test-selected-policy.py` is resolved by validating and reversing
both disjoint qualification chains: memfd leases and shared stdio. Both chains'
negative controls remain active; `ofd-stream-source.json` is byte-identical to main.

The continuation additionally completes the handoff's single-byte Bluetooth
allocation work. Each socket input/output stream retains one byte of scratch
storage. Only users of the same scratch array are serialized; socket close uses
its existing independent lock and interrupts blocked native reads. Array reads
and writes retain their existing path. Tests observe buffers at the native
boundary, distinct concurrent byte values, peer EOF, local close and native errors.

Independent review found and repaired additional regressions: peer EOF is
cleared on successful reconnection, and a late read completion from the old
connection cannot disconnect the new link. Reconnection closes the old native
registry entry, and late failed writes also cannot disconnect the new link.
WASAPI shared event mode now passes
zero for both buffer duration and periodicity, as required by `IAudioClient`.
Only the polled fallback requests a 200 ms buffer. The WASAPI boundary rejects
nonzero event-mode durations so this error cannot silently hide behind fallback.

The Android 17 behavior page, updated 2026-11-11, explicitly includes local socket
close and dropped connections in the target-37 EOF change. This supersedes the
older handoff's narrower clean-peer-only claim. Target 37+ now receives sticky
`-1` after local close, orderly peer EOF and the documented terminating Winsock
results (`WSAENETRESET`, `WSAECONNABORTED`, `WSAECONNRESET`, `WSAENOTCONN`,
`WSAESHUTDOWN`, `WSAETIMEDOUT`). Older targets retain `IOException`; native
argument/resource failures still throw for every target. Windows' native read
boundary is unchanged. Tests cover initial and repeated EOF, array/single-byte
reads, a concurrent local close, each disconnect result, a disconnect first
detected by a write and non-disconnect native errors. Writes still throw on
failure; the subsequent read observes EOF only for a known disconnect and target
37+. The preceding source fails the new local-close assertion.

The shared native memfd guest assertions now verify seal success after the last
unmap, including an original descriptor closed after `mmap`, a duplicated seal
descriptor, partial unmap and `MAP_FIXED` replacement beside an already-dead
tail. The 12 mapping-lease checks also run unchanged against the real Linux
kernel before the modeled Windows checks.

CI later exposed a scheduling assumption in the host lease fixture: all 400
concurrent process rounds could legally favor the parent mapping, so the test
failed its both-orders coverage assertion even though every safety check passed.
Both orders are now required by explicit pipe handshakes between the processes
and condition-variable handshakes between threads. Each ordered case asserts
the successful operation, the other operation's exact `EBUSY`/`EPERM` result,
and the stored seals. The fixture retains all 1,500 concurrent thread rounds,
400 concurrent process rounds and 300 concurrent seal-addition rounds. Process
rounds wait for the child's readiness before release, and all concurrent mapping
versus sealing rounds reject dual success, no winner, incorrect errors and
inconsistent seals.

ASan/UBSan runs passed with normal CPU affinity, one-CPU affinity and a test-only
fork wrapper delaying child execution by 50 ms. That wrapper changes scheduling
only; it does not intercept memfd, mapping, sealing or locking operations. Under
the same delayed-child schedule, the previous unchanged fixture reproduced the
CI failure: zero seal wins, 400 mapping wins, zero dual successes or missing
winners, and only the coverage assertion failed. The corrected race fixture has
19 assertions; its coverage no longer depends on scheduler fairness. A separate
negative control that deliberately omits the production exclusive lease probe
fails eight assertions, including both controlled map-first cases and concurrent
dual-success checks. No production source or CI workflow was changed.

## Executed checks

All checks below passed on the integrated tree. Linux host tests compile or
execute production functions around explicit platform boundaries; they do not
replace ordinary application behavior.

| Check | Result |
| --- | --- |
| `tools/test_mmap_noreplace_contracts.py --sanitize` | 274 checks, zero failures; partial live/dead tails, mapping handle and lease retirement |
| `tools/test_memfd_contracts.py --sanitize` | Linux kernel mapping-lease reference: 12; direct/POSIX/guest routes: 57 checks each; faults: 19; lease basics: 44; process lifetime: 6; race assertions: 19; zero failures |
| `tools/net-tcp-priority/test-selected-policy.py` | PASS; both exact-source transition chains preserved |
| `tools/net-tcp-priority/test-vector-policy.py` | PASS; 33 mutations, 4 repinned property and 7 repinned memfd controls rejected |
| `tools/test_media_player_priming.py --sanitize` | 209 checks, zero failures; unchanged captured-tone tolerances pass |
| `tools/test_capture_stream_selection.py` | 2,061 checks pass |
| `python3 -m unittest discover -s tools/media_capture -p 'test_*.py'` | 12 tests pass |
| `tools/test_bluetooth_socket_contracts.py` | all six cases pass, including buffer reuse/concurrent values, blocked single-byte close and reconnection with a late old read |
| `tools/test_bluetooth_sdp_contracts.py` | bounded pool, lookup coalescing and failure recovery pass |
| `tools/test_bluetooth_watch_contracts.py` | nonblocking startup, state fallback and single watch pass |
| `tools/test_handwriting_callbacks_host.py` | null callbacks and remote callback failures pass |
| Same buffer-reuse assertions against `source-pr381` | Expected failure: per-call input buffer allocation detected |
| Updated target-37 closure assertions against `cbf84bd8` | Expected failure: local close throws instead of returning EOF |

Java checks use Temurin JDK 21.0.13.1+2. Linux native checks use GCC 14 with ASan
and UBSan. This container's tracing prevents LeakSanitizer from starting; local
sanitized commands set `ASAN_OPTIONS=detect_leaks=0`. CI sanitizer settings are
unchanged. Explicit handle/lease lifetime and mutual-exclusion assertions still run.

### Windows compilation

Using the project's pinned `llvm-mingw-20261006` toolchain, both x86_64 and
AArch64 targets compile:

- complete `memfd_seals.cc` and `mman.cc` translation units, with
  `-std=c++20 -Wall -Wextra -Werror -fno-exceptions`;
- `tools/test_capture_failures.py --compile-only`, including the complete capture
  engine, isolated synthetic source fixture, adapters and WASAPI implementation.

This is cross-compilation, not execution on Windows or physical audio hardware.
Full native runtime checks remain the PR's CI integration gate.

## Remeasured MediaPlayer ABBA

Order: candidate, baseline, baseline, candidate. Baseline is current main
`bcfb5044`; candidate is the original round-1 player code integrated with it.
Command: `python3 tools/test_media_player_priming.py --measure`, with
`--source-ref bcfb5044029d486e17138dc417f3ced81524cf3f` for baseline passes.
These are host-fixture wake counts and a controlled-clock measurement, not
Windows CPU measurements or end-to-end application benchmarks.

| Metric | Candidate 1 | Baseline 1 | Baseline 2 | Candidate 2 |
| --- | ---: | ---: | ---: | ---: |
| Paused video wakeups/s | 0.0 | 50.0 | 49.5 | 0.0 |
| Far-frame timing wakeups/s | 19.5 | 98.5 | 98.0 | 19.5 |
| Stalled mixer feeder wakeups/s | 49.5 | 195.5 | 196.0 | 50.0 |
| EOF drain wakeups/s | 9.5 | 99.0 | 99.0 | 9.5 |
| Speed-change position error after 1 s, microseconds | 0 | 1,000,000 | 1,000,000 | 0 |
| Speed-change position error after 2 s, microseconds | 0 | 1,000,000 | 1,000,000 | 0 |

## Contract references and limits

- [memfd sealing](https://man7.org/linux/man-pages/man2/memfd_create.2.html)
- [Win32 LockFileEx lifetime and shared/exclusive locking](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)
- [Android 17 RFCOMM stream EOF change](https://developer.android.com/about/versions/17/behavior-changes-17#connectivity)
- [Win32 recv results and disconnect error meanings](https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-recv)
- [WASAPI event handle contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-seteventhandle)
- [WASAPI shared event-mode initialization](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize)

The memfd fixtures target writable shared mapping ownership and seal races.
They do not certify every mmap/mprotect permission transition. Real NTFS EA
persistence, kernel lock ownership, Windows placeholders, physical Bluetooth and
live WASAPI endpoint behavior require native execution. This scoped round does
not close the complete #56 or #178 roadmaps, and it does not enable the public
blocking file-lock commands that the repository deliberately keeps unavailable.

Round 2 is separately implemented and validated. Follow the linked PR checks for
current integration status; this report records local evidence, not a promise
that queued or running CI has passed.

## Source PR reintegration and independent review, 24 November 2026

The continuation through `b7b128d287519c2db8064a7cf5abd550b744f41a` was
independently inspected and brought back into source PR #381. Main
`e074b67b2f83ce9128c11713171329e9fbbf6739` was then merged, preserving the
original work and the existing continuation commits.

The merge preserves main's newly added `gesher_mmap_fixed_replaces` query for
the ELF loader. Its change to `mman.cc` required rebasing the exact memfd
qualification transition onto that main commit. The transition still reverses
only the three memfd lease files, checks both endpoint hashes, reproduces the
current bytes in the forward direction, and rejects all existing negative
controls. `ofd-stream-source.json` remains byte-identical to current main;
the shared-stdio qualification and all original protected source checks remain
in place. This is source qualification, not a grant of native-runtime approval.

Independent review also found a missing control wake in `CaptureEngine::Submit`:
preparing or starting a second recorder notified `cv_` while the worker could
be blocked in the source's data wait. A quiet endpoint therefore delayed the
control command until a packet or the 50 ms safety timeout. `Submit` now uses
the existing `Wake` path, interrupting either wait and retaining its sticky
restart predicate for the race before a wait begins. Lock ordering remains
engine mutex before wake mutex; the worker releases the wake mutex before
reacquiring the engine mutex.

The new `event-command` case uses the existing explicitly isolated source
boundary whose wait ends only on a signal or interrupt. Both prepare and start
must complete with no injected sample signal. Compiling the complete worker
from pre-fix `f83057ec0bc1eb75126dc5c99ff28c26fb1342f5` with this case fails the
prepare assertion; the corrected worker passes all eight assertions. The
Linux run substitutes only host clock/environment calls and the fixture's
existing WASAPI/priority boundaries; it is not a Windows audio-device run.

Validation on the integrated source:

- The complete capture worker and existing adapter fixtures pass all 20
  production-role cases, including start/stop/release races, reentrant start,
  stale-peer publication, source loss, permissions, event-only waits and the
  new control-command wake case, using the Linux boundaries described above.
- Both x86_64 and AArch64 Windows capture production/isolated fixtures and the
  WASAPI boundary fixture cross-compile with the pinned `llvm-mingw-20261006`
  toolchain and warnings treated as errors. The complete production
  `memfd_seals.cc` and `mman.cc` also cross-compile for both architectures.
- ASan/UBSan memfd fixtures pass, including the real Linux mapping-lease
  reference and all 19 race assertions. Mapping replacement passes 274
  assertions, MediaPlayer priming passes 209 with unchanged captured-tone
  tolerances, and capture selection passes 2,061 with no allocations in
  10,000 presence scans of 64 streams. LeakSanitizer remains unavailable
  under the host tracer; only its detection is disabled locally.
- All six Bluetooth socket cases, bounded/coalesced SDP, Bluetooth watch,
  handwriting callbacks and the 12 capture-role unit tests pass.
- Selected-source and vector policies pass, including 33 transition mutation
  cases, seven repinned memfd controls and 23 shared-stdio negative controls.

CI now executes the existing `event-signal`/`event-wake` cases and the new
`event-command` case in the Windows media track, instead of merely compiling
them. It also runs the new SDP and handwriting host suites. No assertions,
timeouts, required gates or hardware/ART claims are relaxed. Real Windows
execution, physical endpoint/radio behavior and exact-head CI remain the
integration gates; host fixtures and cross-compilation cannot establish them.

An independent second reviewer then reproduced an arithmetic error in the new
video deadline wait: a timestamp of `INT64_MAX / 10` microseconds at speed 0.1
overflowed the floating-to-`int64_t` conversion before the wait was bounded.
The calculation now uses double precision and clamps the delay to the existing
500-to-50,000 microsecond interval before integer conversion. The production
Playback regression presents the first frame, verifies the distant frame stays
pending with a 50 ms recheck, and interrupts the wait on shutdown. The complete
214-check player suite and unchanged captured-tone gates pass with ASan, UBSan
and explicit `float-cast-overflow` instrumentation; the same fixture at
`89119647964a31b78013c6071531b4e2b65fe573` fails at the original conversion.

Main subsequently advanced to `5b2af6e4e5de5525791c304de21c2c6a77aebcd2`
and was merged without conflicts. Its thread-watch, IME and updated OFD
qualification are preserved unchanged. That advance did not change any of the
three memfd qualification sources, so their exact transition remains anchored
to `e074b67b`; no new qualification exception or digest update was needed.
