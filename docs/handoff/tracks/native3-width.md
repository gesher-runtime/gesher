# Native3: verified character-width stage

Correction now verified in full ci 40430425743 at b3ecc85; see
native3-width-clock.md. The historical shared expectations assumed signed wchar32 for both
ABIs. The real Android arm64 target uses unsigned wchar32, so the high-bit
invalid values need different pinned wcwidth/wcswidth outcomes. The signed
x86_64 full-code-point proof below remains valid; the previous ARM tests do
not prove that missing distinction. The roadmap closure was withdrawn;
the independent unsigned oracle and actual per-ABI whole-range guest tests
now pass in the later full run. The evidence below remains historical and
must not be cited as the corrected ARM-width proof.

Source 581e88f4d63896ab648994772832cbf327e61c55, full ci 40404678100.
Root personally read every job (22 success/six skipped), both zero-failure
Windows summaries, actual width/classification/NDK ABI and APK PASS lines,
installed window/input/uninstall cleanup and both nine-app e2e logs with
required Puzzles/TIC-80 and 0 unexpected failures. No rerun was requested.

The production bridge follows android-16.0.0_r4 wcwidth.cpp's exact decision
order over the pinned ICU4C data: signed wchar32 inputs, NUL/control including
U+00A0, mark/format/soft-hyphen handling, Hangul before default ignorability,
special CJK ranges and East Asian Width. The original upstream license is
retained. wcswidth preserves the pinned bounded/NUL/error behavior.

The independent unmodified bionic ICU4X wrapper and exact 2.0.0-beta2 lock
produce digest 566ac5b76f65a70b over every value 0..0x10ffff and four invalid
sentinels. Initial reference ci 40403945297/f8871df and locked reproduction
40404677936/581e88f were read in full; all old 14 property/case digests remain
unchanged. Both Windows host executables match the width digest through the
actual System V shim. Both x86_64/arm64 dalvikvm paths and four APK/mode cases
per runner pass 33 boundary rows, supplementary string bounds, Hangul/mark
combination, NUL termination and invalid/control rejection.

Test jobs 121879299676/121879299471, package 121879299423 and e2e
121880189107/121880189190 supply the full proof. Existing Bitmap/Minikin/
Process and classification/case tests also pass. The package smoke includes
installed oatdump, boot image, fonts, actual app input and complete uninstall.

This stage predates the cloud-profile/exec3 integration. The current wip/native3
continuation merges that newer integration and adds ART CPU-clock patch 0572;
the combination needs its own full CI before integration. The frozen
wip/native3-width branch preserves the original width source. It does not
verify CPU clocks, collation/conversions, binary128 or remaining NDK/POSIX.
Wide/multibyte ABI completion is delegated to native3-wide/ADR0073; root keeps
the clock and remaining native3 scope. All compilation/runtime fixtures now
run in CI only.
