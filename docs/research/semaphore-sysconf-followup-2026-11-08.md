# Semaphore configuration query follow-up — 2026-11-08

This check continues the native audit after PR #8 commit `79567dad`.
It concerns one supported semaphore limit, not additional capability claims.

## Finding and correction

Android's `_SC_SEM_VALUE_MAX` selector is `0x36`. Pinned bionic implements
`sysconf(_SC_SEM_VALUE_MAX)` by returning `SEM_VALUE_MAX`, whose value is
`0x3fffffff` (1,073,741,823). Gesher already enforces that maximum in `sem_init`
and `sem_post`, but its bionic `Sysconf` treated selector `0x36` as unsupported
and returned `-1`. A native application discovering the supported semaphore
limit therefore received an answer inconsistent with the actual implementation.

`Sysconf` now returns the implemented maximum for that Android selector. The
single constant moved from `extra_sys_bionic.cc` into the existing shared
`shim_internal.h`, so query, initialization and overflow enforcement use the
same definition. No other sysconf capability or limit was changed.

Primary sources, all pinned to `android-16.0.0_r4`:

- [bits/sysconf.h: `_SC_SEM_VALUE_MAX` is `0x0036`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/bits/sysconf.h)
- [sysconf.cpp: return `SEM_VALUE_MAX`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/sysconf.cpp)
- [limits.h: the semaphore maximum](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/limits.h)
- [semaphore.cpp: initialization and overflow bounds](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/semaphore.cpp)

## Regression and executed evidence

The shared public semaphore fixture obtains `sysconf` through `dlsym`, verifies
the Android selector's result and errno preservation, and retains an invalid
selector/error control. It then uses the queried value for its real `sem_init`,
`sem_post`, `sem_getvalue` and concurrent overflow checks. The fixture deliberately
uses Android selector `0x36`, not the different host-libc number. Its Android build
also rejects an unexpected header selector at preprocessing time. The same
fixture is already included in the real ELF libc test suites.

The host harness now compiles the actual production `Sysconf` body and its
actual symbol registration together with the production semaphore functions.
Only the unused `GetSystemInfo` boundary is inert; unrelated memory/processor
query boundaries abort if reached. Thus this check does not substitute a
handwritten sysconf answer or pretend those Windows queries ran on Linux.

Executed command:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_bionic_semaphore_contracts.py --sanitize
```

**Result: 76 assertions passed, zero failures**, comprising the previous 73
semaphore/deadline assertions plus three query assertions. AddressSanitizer and
UndefinedBehaviorSanitizer were enabled; leak detection was disabled as in the
existing host harness.

Negative control: supplying the unmodified `79567dad` `posix_bionic.cc` through
`--posix-source` fails the new advertised-maximum assertion. The other query
controls and existing wait/deadline groups pass. This isolates the missing
production query rather than a fixture-only model change.

Both complete changed translation units (`posix_bionic.cc` and
`extra_sys_bionic.cc`) cross-compiled with `-Wall -Wextra -Werror`, the repository's
normal Windows definitions and the exact pinned
`llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64` toolchain, targeting both x86_64 and
native ARM64. The shared fixture also passes strict C11 syntax checking, the
Python harness compiles, and `git diff --check` is clean.

These are local host and compilation results. Real Windows/ART or translated
ARM64 ELF execution remains a CI runtime gate; no such run is claimed here.
