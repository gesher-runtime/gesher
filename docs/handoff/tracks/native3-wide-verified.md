# Native3-wide: verified pinned wide ABI stage

Full source 57782e5896497cdd5a5f32779bdb73dcb9edb2bc, ci 40425393716,
attempt 2. Root read all 28 conclusions (22 success/six skipped), both
zero-failure summaries, all six actual test66 ABI/mode PASS lines per
runner, actual package window/input/uninstall and both required e2e logs
(zero unexpected failures). Attempt 1 failed only known badssl TCP-connect
timeouts; the same-source failed-job retry passed.

Scoped reference ci 40425021540 at eb9f964 verifies licensed/hash-checked
pinned sources and 248 conversion/state/boundary/LP64 assertions, plus
production locale/guest objects under pinned llvm-mingw/-Werror and both
NDK library builds. Root inspected all three job logs. Floating grammar
uses a host narrow converter and wide-time uses a host narrow formatter;
this is not an independent full Android gdtoa/tzcode implementation.

Runtime tests cover actual x86_64 and unsigned-char/wchar ARM64 library
entries through dalvikvm and APK single/device modes. Pinned asymmetric
encoder/decoder behavior, C11 state/NUL quirks, reserved state bytes,
bounded source/end-pointer movement, LP64 numeric limits, C locale/CHAR_MAX
layout, raw C collation and UTF-32 wide-time buffers are retained. They
do not substitute a stricter Unicode policy for baseline behavior.

Attempt-2 tests: 121953896798 (2022), 121953896834 (2025). Package
121953897870 verifies installed oatdump/image/fonts, install/dexopt,
shortcuts/association, real window/input/resize and complete uninstall.
E2e 121953898663/121953897741 passes all nine apps including required
Puzzles/TIC-80, four transport fixtures and association/shortcut launches.
These successful package/e2e logs retain their original attempt-1 payloads;
the retry's two Windows test payloads are new and were separately inspected.

Only docs may be added to frozen wip/native3-wide-verified. Root must
combine newer integration and the corrected width/clock prerequisites
before final acceptance. Binary128 conversion/libm, wide scanf/stdio,
generic POSIX and remaining native3 coverage stay open. No local native
or Java compiler/fixture ran; no main/release operation is implied.
