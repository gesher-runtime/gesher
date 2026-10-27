# Native3: verified per-ABI width and ART CPU clocks

Source b3ecc85efd95caa7403a4c02516a6dfc3779803a, full ci 40430425743,
attempt 1. Root personally read all 28 job conclusions (22 success/six
skipped), both zero-failure Windows summaries, actual scoped PASS lines,
package install/window/input/uninstall and both required nine-app e2e logs
including Puzzles/TIC-80 (zero unexpected failures). No retry was requested.

## Corrected width evidence

The earlier common signed-wchar oracle was wrong for high-bit ARM64 input:
the actual Android ARM64 target uses unsigned wchar32. Native3-wide's NDK
macro audit exposed the mistake. The roadmap closure was withdrawn before
this correction; ci 40404678100 remains valid signed-x86 evidence, not the
corrected ARM proof.

The unmodified pinned bionic ICU4X wrapper and exact beta2 lock produce
separate whole-range width digests: signed 566ac5b76f65a70b and unsigned
3650764a32c5bf03. Root read the independent reference in ci 40423333599 and
its locked reproduction 40426388584. All 14 original classification/case
digests are unchanged. Both references include 33 boundary rows.

The ARM System V guest override preserves unsigned wchar values before
calling the shared pinned-ICU implementation; out-of-range high-bit values
keep the pinned unsigned result. Actual NDK function calls compute the
entire 0..0x10ffff plus four-sentinel digest separately in each ABI. Both
dalvikvm ABI cases and all four single-process/device APK cases pass on
each Windows runner, including bounded supplementary strings, NUL/error
behavior and each ABI's high-bit width/wcswidth results.

## CPU clocks and retained gates

ART patch 0572 selects existing CPU-clock paths on Gesher and returns an
explicit sentinel on clock failure. The linked production-artbase fixture
independently brackets current-thread/process time with GetThreadTimes and
GetProcessTimes. It verifies worker identity, the waiting caller's own clock
and process CPU accumulation while the worker runs; all five checks pass
on both Windows versions. This does not implement non-current pthread clocks.

The independently verified startup input is included. Original image-size
and startup gates pass unchanged: interpreter profile/speed 550/591 ms and
JIT 432/442 ms on 2022; interpreter 623/642 and JIT 480/486 ms on 2025.
The bounded AAC prepare observation changes no player state, HRESULT,
waiting/assertion or tone requirement. Passing this run does not prove the
cause of an earlier AAC timeout or a camera-performance improvement.

## Exact jobs and integration boundary

Tests: 121949111771 (2022), 121949111803 (2025), both zero failures.
Package 121949111731 checks installed oatdump/image/fonts, install/dexopt,
real window/input/resize, associations/shortcuts and complete process/data/
toast/Start menu uninstall cleanup. E2e 121949845796/121949845784 pass all
nine apps, four transport fixtures and association/shortcut launches.

Frozen wip/native3-width-clock adds documentation only above the verified
source. Root must satisfy the pending integration rule-g gate, compare the
merged code against this verified source and run combined CI if it differs.
Monotonic elapsed/deadline clocks belong to native3-time/ADR0075; wide/C11/
collation/numeric/time conversions belong to native3-wide/ADR0073. Native
allocator accounting, cross-thread clocks, binary128 libm, generic POSIX,
descriptor inheritance, JVMTI/linker namespaces and remaining platform APIs
remain explicit native3 work. No full NDK-coverage or release claim is made.
