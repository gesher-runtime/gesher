# File allocation validation follow-up, 2026-11-08

The inspected PR #8 implementation in `compat/posix/src/system.cc` added
signed `offset + len` without checking overflow, accepted negative/zero
ranges when the resulting end did not exceed the existing size, and returned
success on a read-only descriptor when no growth was needed. CRT failures
could also change `errno` even though `posix_fallocate` returns its error
number directly.

## Repaired contracts

The implementation now validates the nonnegative offset, positive length and
representable end before performing the addition. It verifies the actual
handle's write-data rights through the existing native handle-access query;
the generic `F_GETFL` answer is not treated as evidence of permission. Pipes
and non-disk handles are rejected with their actual unsupported-type error.
The descriptor publication/close guard holds the selected file object stable
through the existing size query and conditional CRT growth. Actual CRT errors
are translated, and the entry value of `errno` is restored on every return.
The existing `fallocate(..., mode=0, ...)` wrapper converts a failure to its
different `-1`/`errno` convention.

This retains the actual existing zero-filled file-growth operation and fixes
its argument/access/error handling. It does **not** establish the full
physical range-reservation guarantee of Android/POSIX `posix_fallocate`.

References:

- [Pinned bionic posix_fallocate implementation](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/posix_fallocate.cpp)
- [POSIX posix_fallocate specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_fallocate.html)
- [Microsoft _chsize_s: actual file resizing and zero extension](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/chsize-s)

## Why the remaining backend gap is explicit

A candidate implementation using Windows `FileAllocationInfo` followed by
`FileEndOfFileInfo` was rejected during independent review. Both are absolute
assignments: another handle can extend the file after a size query, and the
later assignment can truncate the other writer's data. A second query does
not eliminate that race. The descriptor-lifetime guard does not serialize
all writers, mappings or independent processes.

[Microsoft's allocation structure documentation](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_allocation_info)
explicitly describes the truncation effect of setting allocation below EOF.
Creating a mapping can extend a file without that particular absolute-size
assignment, but it requires additional handle rights and does not guarantee
backing space for sparse/compressed files; see
[mapping creation](https://learn.microsoft.com/en-us/windows/win32/memory/creating-a-file-mapping-object)
and [mapped file I/O](https://learn.microsoft.com/en-us/windows/win32/memory/reading-and-writing-from-a-file-view).
The candidate is not shipped and is not counted as a completed capability.

The existing CRT size-query/growth sequence itself still lacks a complete
concurrent-resize contract. Sparse/range preallocation, all nonzero fallocate
modes, and a safe exclusion/atomic-growth backend remain implementation work.
Logical growth and zero-filled reads must not be described as proof of those
guarantees. The accompanying storage-service report makes the same boundary.

## Executed tests and CI integration

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_file_allocation_contracts.py --sanitize
```

**40 assertions pass:** 16 real-file growth/content/offset checks, 9 argument
and descriptor checks, 4 overflow checks, and 11 controlled failure checks.
The production function is compiled verbatim. The host envelope uses real
Linux descriptors and files for handle queries, length and growth, and injects
access-query, size-query and disk-full failures. The starting PR fails all
four groups; UBSan reproduces the signed end overflow. These results do not
prove Windows physical allocation or concurrent-resize behavior.

The 29 public-call assertions join the existing Windows x86-64/translated
ARM64 ELF libc test. The complete changed Windows translation unit also
cross-compiles with strict warnings for x86-64 and native Windows ARM64 using
the exact CI llvm-mingw release. Cross-compilation is not execution.

The TCP selected-source preservation gate now pins the complete changed
`system.cc` SHA-256 and separately checks byte equality before and after the
single modified function (allowing only its new `<limits>` include). The
TCP, fcntl, vector-I/O, supplier and workflow portions retain their earlier
exact bytes; the gate does not permit arbitrary future edits in the function.
The host sanitizer regression is added to the ordinary CI preflight.
