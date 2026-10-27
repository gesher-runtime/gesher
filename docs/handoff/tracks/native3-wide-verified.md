# Native3-wide: verified pinned wide ABI stage

Combined source 7302b20432e9b31e30897ced7b372f0e835c4603, full ci
40439802052, attempt 1. The scoped agent personally read all 28 conclusions
(22 success/six skipped) and all 22 successful job logs: both final zero
summaries, six actual test66 ABI/mode PASS lines per runner, corrected signed/
unsigned whole-range NDK widths/33 boundaries and ART CPU-clock checks.
For example, both runners print:

    PASS  native3-wide (arm64, device): pinned multibyte/C11 states, bounds, LP64 numbers, locale aliases, C collation and UTF32 time

Tests 121990064784/121990064798 each end with 0 test(s) failed. Package
121990064884 supplies 23 actual installer PASS lines, including installed
oatdump/image/fonts, install/dexopt, associations/shortcuts, real window/
click/keyboard/wheel/resize input and complete uninstall cleanup. E2e
121990574707/121990574682 each pass all nine apps including Puzzles/TIC-80,
four transport fixtures and association/shortcut launches, ending with
0 unexpected failures. No retry was required.

Same-source scoped ci 40439801814 reproduces all 248 pinned reference cases,
actual upstream source hashes, production locale/guest -Werror and both NDK
compilations; all three actual logs were read. Source 7302b20 combines
current-at-dispatch integration 2f8ed7a and corrected width/CPU c8f30ec,
retaining both ARM localeconv and width overrides. Fourteen baseline entries
gain only scoped verification metadata; historical counts/owners stay intact.

Only docs advance above this source. The frozen predecessor ref below is
unchanged at e7c1b06. This source predates root's verified 0573/time68 merge;
the separate ARM FPCR candidate includes current integration 8922a3e and
these wide prerequisites in full ci 40444486511 at e0f9601, still pending.
Root alone handles integration/main/release. Binary128, wide stdio/scanf,
full narrow gdtoa/tzcode and other native3/POSIX contracts remain open.

## Earlier frozen prerequisite

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
