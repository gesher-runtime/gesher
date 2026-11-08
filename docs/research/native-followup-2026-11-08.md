# Native API follow-up, 2026-11-08

This pass starts from PR #8 commit
`3843241968edfd124d3b9596c0e6381f45f5c381`, after the initial allocation,
group/lock and socket-control repairs. It checks additional absent public
functions and implemented paths with incorrect failure/deadline behavior.

## Missing directory functions

The shim did not register **`scandirat`, `scandirat64`, `telldir` or
`seekdir`**. The pinned Android headers expose the scanning forms from API 24
and the cookie forms from API 23. An app importing these functions could
reach the ELF loader's unresolved-function abort path even though ordinary
directory enumeration already worked.

The new scanning entry points open the requested directory relative to the
supplied descriptor and use the existing bionic directory adapter. They close
the newly opened descriptor if `fdopendir` fails, preserve the actual error,
and retain the caller's parent descriptor. `AT_FDCWD` follows ordinary
`scandir` behavior, including virtual directories.

Directory cookies now track the enumeration position in a signed 64-bit field
owned by the opaque directory stream. `telldir` returns that position;
`seekdir` rewinds and replays entries to restore it. The implementation avoids
narrowing Android's LP64 `long` to Windows' 32-bit `long`. This is a functional
enumeration implementation, not an empty-result placeholder. As with the
underlying Windows enumeration, the tests do not establish snapshot behavior
when directory contents change concurrently.

References:
[pinned Android directory declarations](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/dirent.h)
and [pinned directory implementation](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/dirent.cpp).
The neighboring `preadv64`, `pwritev64`, `lseek64`, `truncate64`, `ftruncate64`,
`fallocate64`, `posix_fadvise64` and `posix_fallocate64` exports were already
present; they were not counted as missing or replaced.

## Directory failure ownership

`scandir` dereferenced an unchecked allocation for each copied entry, used a
throwing C++ vector in the no-exceptions runtime, and copied into an unchecked
result allocation. Failed enumeration could also return a successful partial
result. The repair uses checked C allocations for both the entry list and each
entry, frees every accumulated allocation on failure, closes the stream,
preserves the error through cleanup, and leaves the caller's output untouched.

The reader clears errno immediately before each enumeration operation, so a
filter callback changing errno cannot turn EOF into a false failure. The
Windows backend now distinguishes `ERROR_NO_MORE_FILES` from actual
`FindNextFileW` errors rather than treating every failure as EOF. These changes
cover the scanner's list/entry allocations; they do not establish allocation
failure recovery for every C++ object in the directory/path backend.

## Semaphore limits and distant deadlines

The implementation accepted counts above Android's `SEM_VALUE_MAX` and
unconditionally incremented the counter. It could exceed the documented
maximum or eventually wrap to zero. Initialization now rejects out-of-range
counts with `EINVAL` without modifying storage. Posting uses compare/exchange
to return `EOVERFLOW` at the maximum without changing the count, including
concurrent posters. The maximum is the pinned Android value `0x3fffffff`, not
the host libc's different limit.

Both bionic absolute-deadline helpers multiplied signed 64-bit seconds into
signed 64-bit nanoseconds before checking or clamping. Valid dates outside
that narrower range caused undefined overflow and could become an immediate
timeout. They now perform the arithmetic in 128 bits before selecting a
finite Windows wait interval. Tests cover realtime/monotonic clocks, expired
and equal deadlines, nanosecond-to-millisecond rounding, the saturation
boundary and both signed `time_t` extremes. Untimed waits remain separate.

References:
[pinned semaphore implementation](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/semaphore.cpp),
[pinned semaphore maximum](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/limits.h),
and [pinned time validation](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/private/bionic_time_conversions.h).
This does not implement cross-process semaphore wakeups or complete signal
interruption behavior, nor does it change every other relative-time adapter.

## Executed regressions

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_bionic_directory_contracts.py --sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_bionic_semaphore_contracts.py --sanitize
```

- **72 directory assertions pass.** They include actual exported function
  lookups, filtered/sorted results, descriptor-relative scans, cookie/EOF
  restoration, all 42 allocation failure points in a 40-entry scan, reader
  failures and descriptor cleanup. Allocation failures are injected at the
  linker boundary; real Linux directories witness enumeration and ownership.
- **73 semaphore/deadline assertions pass.** They cover counter bounds and
  concurrent posting, real waits with controlled producers, and 48 controlled
  deadline/clock cases. Linux futexes implement the host envelope's wait/wake
  primitives. The production functions and public registrations are compiled
  verbatim, rather than rewritten for the tests.
- The shared public fixtures compile as C11 with `-Wall -Wextra -Werror` and
  join the existing x86-64 and translated ARM64 ELF libc suites.
- Negative controls against the starting PR reproduce missing directory
  exports, a null allocation store caught by UBSan, false success after read
  errors, eight semaphore limit failures and signed time overflow in both
  deadline helpers. They fail all four directory groups and all three
  semaphore/deadline groups.

The Linux host checks are not Windows execution, translated ARM64 proof,
process-shared semaphore support, or a leak-sanitizer result. AddressSanitizer
and UndefinedBehaviorSanitizer are enabled; only LeakSanitizer is disabled
because this executor cannot perform its process-state inspection. Full
Windows enumeration, descriptor integration and `WaitOnAddress` behavior
remain covered by the native integration gate, not by these host substitutes.
