# Binary128 parsing: N39 first increment

This change replaces the `double` conversion in `strtold`, `strtold_l` and
`wcstold`, and adds `wcstold_l`. The final result is IEEE binary128 with 113
significant bits, including the full exponent and subnormal range. The
implementation preserves the pinned Android parser's actual grammar, payloads,
end pointers, range errors and directed rounding behavior.

This is a parsing increment for issue #56 N39. It does not close N39. Binary128
formatting, scanf and long-double math still have separate implementation work.
The integrated change includes the ordinary ARM64
[numeric return and rounding adapter](arm64-binary128-return-2026-11-18.md).
Actual combined-tree Windows execution remains a required qualification gate.

## Source and licensing

Production compiles the unchanged gdtoa sources from the existing AOSP checkout
at `android-16.0.0_r4`, Bionic commit
`f22516cbc67e81c13cf943ce02f01b8929a98726`, tree
`2785efe04f43333ae6aaa11719321cf18f535398`.
`aosp.lock` adds that parser directory to Bionic's sparse checkout. This is not
a new runtime download or a host CRT long-double dependency.

The primary contract sources are
[Bionic strtold](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/strtold.cpp),
[the gdtoa parser](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/upstream-openbsd/lib/libc/gdtoa/strtorQ.c),
[the multiprecision correction](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/upstream-openbsd/lib/libc/gdtoa/strtodg.c),
[wcstod/wcstold](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/wcstod.cpp)
and [parsefloat](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/stdio/parsefloat.c).

`probes/native3/binary128/sources.json` records SHA256 for all eleven compiled
source/header inputs. `prepare_sources.py` verifies those hashes and can fetch
missing files by immutable commit. Original Lucent/David M. Gay permissive
notices remain in every upstream file. Their complete notice blocks are also
included in `BIONIC-GDTOA-NOTICES.txt`, the Windows artifact and installer.
Gesher's adapter code is Apache-2.0. No GPL/LGPL implementation is used.

SoftFloat alone was not selected because it does not supply decimal parsing.
A wider new math dependency was unnecessary for this slice: Bionic already
uses `strtorQ` for its LP64 public parser, including its observable edge cases.

## Production behavior

`ParseBinary128` calls the pinned parser with a 113-bit `FPI`, writes four
32-bit words and copies all sixteen bytes to the result. The algorithm uses a
binary64 approximation internally, then corrects it with arbitrary-precision
integer arithmetic; there is no final-value conversion through `double` or the
Windows native `long double`. Values such as `1 + 2^-112`, `1e4000`, `1e-4000`
and the smallest binary128 subnormal retain their full representation.

The platform headers select little-endian binary64, 32-bit gdtoa words and
Android LP64 NaN bits independently of Windows LLP64. Static assertions check
the host assumptions. Every compiled upstream helper/table is prefixed
`gesher_gdtoa_`, so it does not replace a CRT conversion routine.

The upstream free list, private allocation arena, powers-of-five cache and
hex digit table are shared. As in Bionic, the parser is compiled with
`MULTIPLE_THREADS`; `compat/bionic/gdtoa/thread_private.h` maps upstream's two
dtoa locks to hooks in `binary128_parse.cc`, and those locks are held only
around free-list/arena updates (lock 0) and powers-of-five cache growth
(lock 1). Simultaneous conversions therefore run in parallel; a 200,000-digit
conversion (about 1.3 s here) no longer blocks every other caller.

Two upstream details need more than the per-structure locks, and are handled
without changing upstream function bodies:

- `pow5mult` reads the 5^(2^k) chain without a lock, while a grower publishes
  a new node before clearing its `next` field, and `Balloc` does not reset
  `next` on a recycled Bigint. Each conversion holds a shared gate, and lock 1
  takes it exclusively, so no other conversion reads the chain while it
  grows. Taking it exclusively waits for every conversion in flight, and
  conversions that start meanwhile can queue behind that request. Before the
  first conversion, inside the same `std::call_once` that fills the hex
  table, the chain is therefore grown to 5^(2^15) (nodes 5^4 through
  5^32768, about 20 KiB, about 1.5 ms once). `strtodg` passes `pow5mult` the
  magnitude of the decimal exponent less the fraction-digit count, with the
  explicit exponent capped at 19999, so this covers every input with fewer
  than about 45,000 fraction digits: the whole binary128 range, including
  the exact 16,494-fraction-digit expansion of the least subnormal and the
  halfway points below it. Only a longer digit string still grows the chain
  after that, once per new node and process. That conversion then waits for
  the conversions in flight, and other conversions that start meanwhile wait
  for it and for the growth itself; while another long conversion is in
  flight, that stall can last until it finishes. With no other long
  conversion running, a 200,000-digit conversion's growth stalled short
  conversions for about 15 ms here.
- `Balloc` and `pow5mult` return NULL after an allocation failure with their
  lock still held; with Bionic's mutexes that lock stays held. Lock 0 is
  re-entrant for the failing thread, and the parse gate releases any lock
  still held when the conversion returns. The allocation failure itself is
  still reported as upstream does.

The hex digit table is filled once with `std::call_once` before any
conversion, instead of upstream's unsynchronized lazy check.
`binary128_concurrency` keeps the eight-thread exact-bits check and adds a
contention check: short conversions must complete while a 200,000-digit
conversion is still running, with exact results for both. The previous
whole-parse mutex fails it (no short conversion completes during the long
one). A first-use check runs before any other conversion in the
process, without a serial run that grows the chain: while a 200,000-digit
conversion runs, a first `1.000000000000000000000000000000000000001e4900`
conversion and a stream of short conversions must each finish within a
quarter of the long conversion's time and before it ends, with results equal
to serial ones. Without the pre-growth, the `1e4900` conversion and short
conversions waited about 0.9–1.0 s for the long one and the check fails; with
it, the `1e4900` conversion took under 0.1 ms and the slowest short
conversion about 15 ms (the long conversion's own growth).
No regression test reproduces the stale `next` race that the gate prevents:
it needs a reader to follow a node in the few instructions between its
publication and the clearing of its `next` field, during one of a handful of
growths per process, so replacing the gate with a plain lock 1 does not fail
any deterministic test. The allocation-failure release path is not exercised by a test.
Allocation and numeric errors remain upstream behavior.

All supported Bionic locales use the C numeric syntax. `USE_LOCALE` is not
enabled, and `_l` aliases ignore their locale argument as Bionic does. Host CRT
locale changes cannot change the decimal separator.

The wide wrapper retains UTF-32 input, Unicode whitespace and Bionic's exact
candidate alphabet. For binary128 only, it commits the same prefix as
`parsefloat` before numeric conversion. This matters for `nan(1`, `nan(1+2)`
and invalid negative inputs such as `-.`: direct narrow gdtoa and Bionic's
wide grammar have different payload, sign-zero or end-pointer results.
The new scanner is compared to the verbatim pinned `parsefloat` function body;
the existing binary32/binary64 wrappers are not changed by that template option.

Pinned Bionic has observable range quirks. For example, the ordinary hex
overflow path returns infinity in all four modes, while its errno/exception
behavior differs from decimal parsing. The Android corpus records those
behaviors. The independent mathematical oracle covers finite values across
the range; it does not silently replace Bionic overflow behavior with an ideal
IEEE expectation.

## Floating-point environment evidence

The parser reads the current mode and gives gdtoa an explicit value:
zero=0, nearest/even=1, upward=2, downward=3. It does not change the caller's
mode, erase existing exception flags or synthesize flags from gdtoa status
bits. The ARM64 numeric wrapper establishes the guest's mode for the internal
binary64 operations, preserves native state and merges actual raised standard
exceptions into guest FPSR.

The ordinary Android reference ran on API36 x86_64, fingerprint
`Android/sdk_phone64_x86_64/emu64x:16/BE2A.250530.026.D1/13818094:userdebug/test-keys`.
Its 1,376 records cover 86 inputs, four modes and four public APIs. Every mode
was preserved. All 344 narrow value/end/errno/mode observations agree with the
production backend in native execution, and each `_l` alias agrees with its
plain form. The raw observations and source/payload digests are checked in.

**The emulator does not qualify arithmetic-generated exception flags.** It
reported zero for every parser observation, and a separate ordinary volatile
`1.0 / 10.0` control also reported zero inexact in all four modes even though
the result bits followed directed rounding. Explicit `feraiseexcept` worked,
and the parser retained a preexisting divide-by-zero flag. The control output
and this limitation are retained with the reference metadata.

Consequently, public tests do not require zero inexact for every decimal
conversion. They verify preservation of explicitly seeded divide-by-zero and
inexact flags, unchanged modes and the non-inexact results of these source-
audited cases. A separate native Windows control requires the genuine inexact
flag from gdtoa's binary64 approximation of `0.1`. Exact incidental arithmetic
flags for the entire corpus on physical Android hardware, nondefault FPCR
flush modes/IDC, and trapping environments remain unqualified by this receipt.

## Validation and execution paths

Completed locally on the integrated source:

- 9,024 decimal/hex and rounding pairs match an independent Python integer-
  ratio oracle. Expected bits use no host float, Decimal context or gdtoa.
- 20,086 grammar cases match the pinned Bionic `parsefloat` body.
- Eight concurrent callers complete 80,000 decimal midpoint parses with exact
  bits, unchanged errno and independent rounding modes.
- The actual numeric environment helper passes 192 assertions, including all
  sixteen native/guest mode pairs, actual raised flags and full restoration.
- Clean main-project CMake builds complete all 357 required compile/link steps
  for the production Bionic shim, ELF loader and five Windows numeric targets.
  A separate clean build links the complete production `gesher_arm64.dll`.
  Both use the pinned llvm-mingw Clang 23.1.3 compiler. The AOSP inputs are an
  isolated copy reset to all 123 pinned commits with the current repository
  patches applied and the eleven gdtoa input hashes verified.
- Fresh immutable Dynarmic and fmt archives are verified against all 2,159 and
  131 blobs in their authenticated Git trees. Boost's archive matches its
  publisher SHA256 and the authenticated 1.83.0 release commit. Only the
  existing workflow's compiler compatibility and FPSR patches are applied.
- The full Native2 test library compiles and links with NDK r27c for x86_64
  and arm64 using its existing `-Wall -Werror` policy. The ordinary numeric
  return fixture builds for both ABIs as executables and shared libraries.
  Cross-compilation is not Windows execution.

The separate Android numeric return reference initially failed 32 of its
1,292 checks. Every failure involved the same wide hex input: Bionic's wide
candidate alphabet omits b/c/d, so it stops before the first b. An ordinary
public-API observation confirmed these results in all four rounding modes:

| Input `0x1.23456789abcdep12` | Binary128 bits | Consumed characters |
| --- | --- | --- |
| `strtold`, `strtold_l` | `400b23456789abcde000000000000000` | 20 |
| `wcstold`, `wcstold_l` | `3fff23456789a0000000000000000000` | 13 |

The fixture now expresses the measured expectation per API. It retains every
sample, mode, prior-flag seed and assertion. The corrected Android run passes
all 256 conversions and 1,292 checks. The original failed output, sixteen
observation rows, corrected output, source/payload identities and the same
emulator flag limitation are retained in
`probes/native3/binary128/android-numeric-return.json` and its companion files.
`generate_expected.py --check` binds these observations to the exact fixture
source as well as validating the 1,376-row parsing corpus.

`binary128-parsing.yml` runs the native component checks against the same
production CMake library and the native numeric environment helper. The full
product CI builds `binary128_records`, `binary128_concurrency`,
`binary128_parsing_test`, `arm64_numeric_fenv_test` and
`arm64_numeric_return_host`. The Native2 producer builds the ordinary ARM64
shared fixture. Windows track46 executes those targets, including the exact-
rational oracle on the Windows executable and four guest fixture runs with
different native environments. The host fixture uses the actual ELF loader,
Bionic shim and translator; successful execution requires all four guest
runs and native environments to pass.
Track44 requires the binary128 result from both NDK ABIs through dalvikvm and
both APK modes. That test calls all four public parsers by direct import and
`dlsym`, with C/C.UTF-8 locales, optional end pointers and sticky flags.

The native tests use genuine production symbols; no package exception,
substituted numeric result or test-only production branch is introduced.
The parser-only branch is based on main `e80cd185` plus the exact reviewed PR125
helper-pin tree `f5a46933`. The integrated parsing PR additionally composes the
exact reviewed PR123 dependency and the isolated ARM64 numeric delta.

Actual combined-tree Windows execution, independent QA of that exact tree,
and final PR qualification are still required. This document is not a claim
that those gates, the entire long-double API, or all of N39 have passed.

## Reproduce component checks

```sh
python3 probes/native3/binary128/generate_expected.py --check
python3 probes/native3/binary128/prepare_sources.py /tmp/quad-aosp --fetch
cmake -S tests/binary128 -B /tmp/quad-build -G Ninja \
  -DAOSP_DIR=/tmp/quad-aosp -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/quad-build --parallel 4
python3 tests/binary128/finite_oracle.py /tmp/quad-build/binary128_records
/tmp/quad-build/binary128_prefix
/tmp/quad-build/binary128_concurrency
/tmp/quad-build/arm64_numeric_fenv_test
```
