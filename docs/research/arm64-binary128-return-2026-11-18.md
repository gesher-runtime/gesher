# ARM64 binary128 result delivery and numeric environment

This is the numeric ABI prerequisite for issue #56 N39's `strtold` family.
The integrated change composes the separately frozen parser implementation
and this numeric ABI delta. They require actual combined-tree Windows
qualification before a runtime compatibility claim or PR approval.

## Contract and implementation

Android LP64 `long double` is binary128. The pinned
[AAPCS64 2025Q4](https://github.com/ARM-software/abi-aa/blob/daa7a94ca55973736c0e434a67a6e4bbcd35d7fa/aapcs64/aapcs64.rst)
assigns a quad argument to a 128-bit SIMD register and returns a register-sized
result by the corresponding argument rule. The pinned
[x86-64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/e1ce098331da5dbd66e1ffc74162380bcc213236/x86-64-ABI/low-level-sys-info.tex)
classifies binary128/vector128 as SSE plus SSEUP, using both halves of XMM0
for the result. Gesher's shim already represents its binary128 return as a
16-byte vector to avoid Windows' different C++ `long double` representation.

Previously `gesher_call_host_sysv` stored only the low half of XMM0, and
`Env::CallHost` explicitly set the guest V0 high half to zero. The new private
`HostCall` record appends `out_xmm0_hi` at byte 56. The existing low result at
40 and integer RDX result at 48 retain their offsets. A `movhpd` stores the
additional word, and `CallHost` supplies both words to V0. Shared layout
assertions cover every field and the 64-byte record size. Scalar float,
double and integer return channels retain their existing meaningful bits.

The inherited thunk qualification fixture uses the same private declaration.
Its source manifest includes that header. No thunk prologue, saved register,
stack allocation or unwind instruction changes.

The ordinary dispatcher restores native MXCSR before calling a host function.
Consequently the parser's `fegetround()` cannot read guest FPCR without an
adapter. Four existing architecture-specific libc overrides now wrap
`strtold`, `strtold_l`, `wcstold` and `wcstold_l`. They call the actual shim
providers within `NumericFenvScope`, which:

1. Saves the native C floating-point environment and exact MXCSR.
2. Installs a clear, nontrapping environment and maps guest FPCR rounding to
   the native `FE_*` constants. Both the parser's mode query and its native
   binary64 seed arithmetic see that mode.
3. Maps only standard exception flags actually raised during the conversion
   to ARM FPSR bits, ORing them with the current guest FPSR. Prior guest bits,
   including IDC and QC, remain intact.
4. Restores the native environment and exact MXCSR on ordinary return.

The mode and flag definitions come from pinned Bionic
[`bits/fenv_arm.h`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/include/bits/fenv_arm.h)
and [`fenv-arm64.c`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libm/fenv-arm64.c).
Bionic's pinned
[`strtold.cpp`](https://android.googlesource.com/platform/bionic/+/f22516cbc67e81c13cf943ce02f01b8929a98726/libc/bionic/strtold.cpp)
passes `FLT_ROUNDS` to `__strtorQ`. The backend retains that explicit mode
contract. No translator API version change or cross-DLL state query is needed.

This scope covers standard rounding modes and the standard nontrapping
environment. It does not establish equivalence for additional FPCR controls
such as flush-to-zero, nor add support for binary128 arguments or full-width
returns from guest functions entered via `host_entry`.

## Dependencies and overlap

The source delta starts from published main
`e80cd1854163bd18e4ea5392c25f423184048804`, tree
`bb9983408687409d12fdf2a1f8d7e47ccbd01cf0`, with these reviewed changes composed:

| Dependency | Published commit | Reviewed tree |
| --- | --- | --- |
| [PR125 helper pins](https://github.com/gesher-runtime/gesher/pull/125) | `d73a1e09caf29483a67b609459ad87f8cbc2b8fc` | `f5a46933f7e9e7687a837b8f35c50c61f01cbda5` |
| [PR123 thunk metadata](https://github.com/gesher-runtime/gesher/pull/123) | `69286e4a34c97b80bf82d5478c6a7c0d15361b0a` | `2faa8a37f6cee783f9e775181ff25f8f562d0cf9` |

The composed prerequisite tree is
`61908c8ee000e09ba956c5b5922b0ee5d5235972`.
The open-PR audit covered 57 PR file lists and the relevant patches in
PR85/120/124/116/107/103/98/93. No duplicate conversion-environment or
binary128-return repair was found. PR85 changes the same `HostCall`
initializer's stack-count expression and will need the additional high-word
initializer retained when composed.

## Focused verification

`tests/native/arm64_numeric_return.c` is an ordinary NDK-compiled C program
and shared library. Eight values are exactly representable in binary64,
including values with nonzero bits in each half of the binary128 result.
This separates return delivery from the former parser's precision limitation.
It exercises four APIs, four rounding modes and two prior exception states:
256 conversions, plus float/double/integer scalar returns, for 1,292 checks.
It checks consumed pointers, errno, rounding preservation and prior sticky
flags. It does not require the emulator to report newly raised arithmetic
exceptions.

The original reference incorrectly expected the wide hex grammar to consume
`0x1.23456789abcdep12` exactly like the narrow parser. Actual Android produced
32 failures, and a separate sixteen-row public-API observation showed that
the wide candidate alphabet stops before b. The integrated fixture keeps
every check and records the measured values/end offsets per API. Its
corrected Android reference passes 256 conversions and 1,292 checks; the
initial failure, exact observations and source-bound corrected result remain
in `probes/native3/binary128/android-numeric-return.json` and companion files.
See the [parser evidence](binary128-parsing-2026-11-18.md) for the exact values.

`tests/native/arm64_numeric_fenv_test.cc` tests the actual scoped environment
helper without an outer dispatcher scope that could hide a restoration
failure. All 16 native/guest rounding pairs exercise positive and negative
halfway arithmetic, actual inexact observation, five standard flag mappings,
complete native fenv restoration and exact MXCSR restoration. On native
Linux x86-64, GCC 14.2.0 passed all 192 assertions. This is native helper
evidence, not Windows or translated execution.

LLVM-mingw 20261006 / Clang 23.1.3 compiled the new `guest_libc.cc`, the host
loader harness and the inherited ABI fixture with warnings as errors, and
assembled the production thunk; the standalone Windows fenv helper linked. NDK r27c
compiled x86-64/ARM64 executables and the ARM64 shared library. The thunk's
compiled `.xdata` was byte-identical to the reviewed dependency (SHA-256
`a0043155ae78dc19628c98278510449f125368e423b331a304e7fa8d5aafaef5`).
No new native unwind or fault experiment was needed.

The integrated source additionally completes clean production translator and
main-project ELF/Bionic builds, and rebuilds both Native2 guest libraries.
CI now explicitly builds the two numeric host targets, produces the ordinary
ARM64 shared fixture, and runs both through Windows track46. The fenv helper
also runs in the Linux binary128 component workflow. These build and CI
checks do not establish Windows runtime execution by themselves.

The shared parser's Android API36 reference recorded 1,376 observations with
rounding preserved. Separate emulator controls showed that volatile `1/10`
correctly changes its rounded result while newly raised arithmetic flags
remain zero; explicit `feraiseexcept` and prior sticky flags do work. Thus
zero newly raised flags in that emulator are not a reliable real-hardware
exception contract. The implementation does not manufacture zero flags from
that observation. Integrated native Windows checks and a future reliable
Android hardware exception oracle must remain separately identified.

## Integrated qualification commands

With the NDK r27c toolchain directory in `NDK_BIN`, build the ordinary guest:

```sh
"$NDK_BIN/aarch64-linux-android29-clang" -std=c11 -O2 -Wall -Wextra -Werror \
  -fno-builtin -frounding-math -shared -fPIC -DGESHER_NUMERIC_SHARED \
  -Wl,-soname,arm64_numeric_return.so tests/native/arm64_numeric_return.c \
  -lm -o arm64_numeric_return.so
```

From the same integrated source tree, build `gesher_arm64` using the existing
translator CMake project, and build the main-project targets
`arm64_numeric_return_host` and `arm64_numeric_fenv_test`. Put the resulting
DLL and executables together, then run on actual Windows:

```powershell
./arm64_numeric_fenv_test.exe
./arm64_numeric_return_host.exe ./arm64_numeric_return.so
```

Require exit zero and `NUMERIC_FENV_DONE checks=192 failures=0`;
the production harness must report four successful 256-conversion guest
runs, all four native environments restored, and
`NUMERIC_HOST_DONE runs=4 failures=0`. This harness uses the production ELF
loader, Bionic shim and translator without replacement numeric providers.
Run the parser's complete binary128 corpus on that same integrated runtime
to qualify rounding beyond binary64 precision. Record the exact source tree,
build commands, artifact hashes and Windows runner identity for independent
QA. These integrated runs were still pending when this delta was frozen.
