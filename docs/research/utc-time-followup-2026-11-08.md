# UTC calendar conversion and extreme time differences

## Confirmed public-call defects

At published revision `d4773067a02a418759a2638f277effe84b9987cc`, `gmtime_r`
delegated to `_gmtime64_s`. Its documented range is 1970-01-01 through
3000-12-31. A valid Android call with `time_t(-1)` therefore returned null
and `EOVERFLOW` instead of 1969-12-31 23:59:59 UTC. Native date consumers
could reject historical dates or dereference a result which succeeds on
Android. The `gmtime` and `gmtime64_r` exports shared this implementation.

`timegm` and `timegm64` used the similarly restricted `_mkgmtime64` and
treated every result of `-1` as failure, including the legitimate instant
immediately before the epoch. Separately, `difftime` subtracted signed
64-bit timestamps before conversion to double, causing signed overflow
for sufficiently separated valid inputs.

Primary references:

- [Microsoft `_gmtime64_s` range and errors](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/gmtime-s-gmtime32-s-gmtime64-s?view=msvc-170).
- [Microsoft `_mkgmtime64` range](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/mkgmtime-mkgmtime32-mkgmtime64?view=msvc-170).
- [Pinned Android 16 `gmtime_r`, `timesub`, `time2sub`, and `timegm`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/tzcode/localtime.c).
- [Pinned Android LP64 integer widths](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/stdint.h).
- [Pinned Android time tests](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/tests/time_test.cpp).

## Implementation and deliberate reference differences

`compat/bionic/time_bionic.cc` now converts UTC with Gregorian 400-year
eras and signed floor division. It supports negative Unix times, year zero
and negative years, dates after 3000, and the full range whose signed
32-bit `tm_year` is representable. Unrepresentable years return
`EOVERFLOW`. Successful conversions fill calendar fields, weekday, day of
year, `tm_isdst = 0`, `tm_gmtoff = 0`, and `UTC`. The name agrees with pinned
`gmtload`'s `Etc/UTC` and `UTC0` fallback. Export registration and existing
per-thread result storage are unchanged.

Inverse conversion retains Android's checked int-sized carries from
minutes to hours and hours to days. Year accumulation is explicitly
64-bit, matching Android LP64 rather than the Windows `int_fast32_t`.
It first handles out-of-range seconds after the other fields, then retries
with seconds carried into minutes. The pre-1970 first-attempt
`seconds - 59` boundary is checked without overflow. Only a fully
representable result replaces the caller's structure; failure preserves
all of it. Successful calls preserve incoming `errno`, including result
`-1`; rejected normalization reports `EOVERFLOW`.

The unchanged pinned tzcode was executed as a separate oracle. These
differences are intentional and tested separately from equality checks:

1. Its binary search can leave `EOVERFLOW` after successful `timegm` calls.
   Gesher preserves incoming `errno`; successful value/field equality does
   not imply equal incidental error state.
2. Several early normalization failures return `-1` without setting
   `errno`. Gesher retains the rejection and unchanged input but makes
   `EOVERFLOW` explicit.
3. Pinned checked year assignment writes a truncated year before failure.
   `gmtime_r`'s observed partial output is reproduced and compared.
   In `timegm`, reusing that partial result in its second attempt can
   convert one second beyond the largest year to a date near the smallest
   year, and vice versa. Gesher rejects both real overflows and preserves
   the original structure. It does not reproduce this wraparound bug.

`difftime` now subtracts unsigned magnitudes before converting and applying
the sign. This handles opposite extremes while retaining a one-second
difference between adjacent large timestamps. Casting both timestamps
first would lose that difference. Pinned
[`libc/bionic/time.cpp`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/time.cpp)
also has signed `t1 - t0`; its undefined overflowing result is not used as
an oracle. Tests instead use exact 128-bit subtraction.

## Executable evidence

The host script extracts the actual production UTC type, constants,
storage, and function bodies. Default GCC ASan/UBSan execution passes
**130/130 assertions**. This includes 20,000 `gmtime_r` and 10,000
normalized `timegm` comparisons with Linux libc, plus 10,000 `difftime`
pairs compared with exact wider arithmetic. Cases cover both signs,
leap centuries, year endpoints, large carries, legitimate `-1`, overflow
and failure-state preservation, and concurrent result storage.

The optional pinned oracle compiles unchanged AOSP tzcode with renamed
symbols and a host `Etc/UTC` file loader. Its input hashes are enforced.
It passes **149/149 assertions**, adding another 20,000 forward and 10,000
inverse comparisons and the explicit boundary/divergence checks. An
independent reviewer reproduced this result. This executes the pinned
calendar code on a host, not on Android or Windows.

```sh
python3 tools/test_utc_time_contracts.py --sanitize

# A platform/bionic checkout at android-16.0.0_r4:
python3 tools/test_utc_time_contracts.py --sanitize \
  --tzcode-dir "$AOSP_BIONIC/libc/tzcode"
```

The managed executor's process inspection prevents LeakSanitizer from
running. Recorded local sanitizer runs used `ASAN_OPTIONS=detect_leaks=0`;
address and undefined-behavior checks remained active. The calendar code
allocates no heap storage. No leak-detection result is claimed.

```sh
python3 tools/test_utc_time_contracts.py \
  --source-ref d4773067a02a418759a2638f277effe84b9987cc
```

This exact old source passes only **56/130**, with **74 failures**. Its CRT
calls use an explicitly identified host facade for Microsoft's documented
range restriction; this is not actual Windows UCRT execution. Old
`difftime` executes unchanged, and the same negative control with
`--sanitize` reports signed overflow for `INT64_MAX - INT64_MIN`.

The shared `tests/native/utc_time_contract_test.inc` is now called by the
real Android `libc_test.c`. Its **125 assertions** use volatile public
import slots so optimized `-O2` builds retain the calls, and also resolve
and call `gmtime64_r` and `timegm64`. Existing full Windows suites run this
fixture as x86-64 and translated ARM64 ELF. Local GNU C11 compilation with
`-O2 -Wall -Wextra -Werror` passed, and its object retains imports of all
four ordinary functions and `dlsym`. No local NDK was available; actual
Android compilation and Windows execution of these new cases remain CI
requirements.

The complete modified `time_bionic.cc` compiled with the exact CI
LLVM-MinGW 20261006 UCRT toolchain for **Windows x86-64 and native ARM64**,
using C++23 and `-Wall -Wextra -Werror`. This verifies the whole unit and
both host ABIs, rather than only the portable extracted body. No source
identity gate or historical certificate was changed by this repair.

## Scope and source identities

Local-time conversion, `mktime`, timezone database integration, and
`strftime`'s existing local `%s` conversion retain their prior behavior.
Their CRT range and timezone limitations are not resolved here. This
patch also does not implement leap-second tables beyond Android's ordinary
Unix UTC calendar. Its scope is the concrete UTC public-call failure and
the adjacent subtraction defect.

| Input | SHA-256 |
| --- | --- |
| Repaired `time_bionic.cc` | `5f0a9d56e4b27b2a48679225a8dd1a12f1e512b71eb050cfc8c38c7c643f402a` |
| Published old `time_bionic.cc` | `49769d0722148b7cfd054f84168eea3dec205013c240d29bd9d43075116f9deb` |
| Pinned `localtime.c` | `94734ca974ae7205dccef40dfcface2c3520ae67c932fb3af7aec1fd47e35b92` |
| Pinned `private.h` | `b6e4dbc3b2b275f222d0fe9ce8761fdf7ccad5102515a30323e2e7ef34d13238` |
| Pinned `tzfile.h` | `87192a964c733091b397860bfb42a2a03dc2c342f9e495ebc8c4d79a1e187a35` |
| Pinned `stdint.h` | `c37028b9bf6b76e9ebe4d2c6ca2cd3bc280b57d9d2593da3bbf2888d9966c3f4` |
| Pinned `time.cpp` | `c15188d901ecb21dafedef583d38786692edb5719981fddeb945c93a92f87811` |
