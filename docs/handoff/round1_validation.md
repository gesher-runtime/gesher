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

Independent review found and repaired two additional regressions: peer EOF is
cleared on successful reconnection, and a late read completion from the old
connection cannot disconnect the new link; WASAPI shared event mode now passes
zero for both buffer duration and periodicity, as required by `IAudioClient`.
Only the polled fallback requests a 200 ms buffer. The WASAPI boundary rejects
nonzero event-mode durations so this error cannot silently hide behind fallback.

The shared native memfd guest assertions now verify seal success after the last
unmap, including an original descriptor closed after `mmap`, a duplicated seal
descriptor, partial unmap and `MAP_FIXED` replacement beside an already-dead
tail. The 12 mapping-lease checks also run unchanged against the real Linux
kernel before the modeled Windows checks.

## Executed checks

All checks below passed on the integrated tree. Linux host tests compile or
execute production functions around explicit platform boundaries; they do not
replace ordinary application behavior.

| Check | Result |
| --- | --- |
| `tools/test_mmap_noreplace_contracts.py --sanitize` | 274 checks, zero failures; partial live/dead tails, mapping handle and lease retirement |
| `tools/test_memfd_contracts.py --sanitize` | Linux kernel mapping-lease reference: 12; direct/POSIX/guest routes: 57 checks each; faults: 19; lease basics: 44; process lifetime: 6; race assertions: 9; zero failures |
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

Java checks use Temurin JDK 21.0.13.1+2. Linux native checks use GCC 14 with ASan
and UBSan. This container's tracing prevents LeakSanitizer from starting; local
sanitized commands set `ASAN_OPTIONS=detect_leaks=0`. CI settings and test
assertions are unchanged. Explicit handle/lease lifetime assertions still run.

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
- [Android 17 RFCOMM stream EOF change](https://developer.android.com/about/versions/17/behavior-changes-17#bluetoothsocket)
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
