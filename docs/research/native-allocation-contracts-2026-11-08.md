# Native allocation compatibility audit, 2026-11-08

The reviewed baseline is `a4dd7242cfed1a08cd8761ae896d63faf7b8cf63`.
This is a bounded native allocator correction, not a complete NDK conformance
result or evidence that an arbitrary APK cannot crash.

## Confirmed defects and corrections

| Contract | Baseline failure | Correction |
| --- | --- | --- |
| Aligned allocation size | `size + alignment` could wrap. For example, `memalign(4096, SIZE_MAX)` could return an apparently successful, tiny allocation. A caller trusting the requested size could corrupt the heap. | Reject addition overflow with `ENOMEM` before allocating. The same helper covers `posix_memalign` and the existing `aligned_alloc` export. |
| Aligned `realloc` failure | The old allocation was removed from the ownership table and freed even when replacement `malloc` returned null. Normal caller recovery then accessed or freed released memory. | Keep the original storage and ownership record until allocation and copy succeed. |
| `reallocarray` availability | No symbol was registered, although Android exposes it from API 29. A required import could reach the loader's unresolved-function path. | Register a System V entry point, reject multiplication overflow, and reuse the corrected `realloc` path. |
| Zero-sized `realloc` | An aligned non-null input was freed and replaced by a one-byte allocation. | Match bionic: a non-null input is freed and returns null; `realloc(nullptr, 0)` returns a freeable allocation. |
| `posix_memalign` errors | Failed underlying `malloc` changed `errno`, although this interface returns its error number. | Preserve `errno` and leave the caller's output pointer untouched on failure. |
| Zero-dimensional `calloc` | Replacing each zero argument by one turned `calloc(0, SIZE_MAX)` and its reverse into enormous allocations that failed, instead of the requested zero-size allocation. | Normalize the whole empty request to one zeroed byte; preserve checked CRT multiplication for two nonzero dimensions. |

References are Android's [malloc API declarations](https://android.googlesource.com/platform/bionic/+/android16-release/libc/include/malloc.h),
[malloc regression tests](https://android.googlesource.com/platform/bionic/+/refs/heads/main/tests/malloc_test.cpp)
(`memalign_overflow`, `realloc_overflow`, `realloc_0`,
`realloc_nullptr_0`, `reallocarray_overflow`) and
[bionic API history](https://android.googlesource.com/platform/bionic/+/refs/heads/main/docs/status.md).

## Regressions and observed evidence

`tests/native/allocation_test.inc` is included in the existing `libc_test.c`
fixture, so the normal native suites exercise the same cases as x86-64 and
translated ARM64 ELF calls. Dynamic lookup of `reallocarray` checks its export
without raising the fixture's API 26 compilation floor.

The lightweight host check compiles the production memory section and its
registration table **verbatim** from `compat/bionic/shim.cc`, with the same
shared fixture. Its only heap API adaptation is Windows `_msize` to host
`malloc_usable_size`. It retains the System V call convention and builds with
exceptions disabled, as the runtime does.

```sh
python3 tools/test_bionic_allocations.py --sanitize
git show a4dd7242cfed1a08cd8761ae896d63faf7b8cf63:compat/bionic/shim.cc > /tmp/shim-before.cc
python3 tools/test_bionic_allocations.py --source /tmp/shim-before.cc --sanitize
```

The corrected source passes **35 assertions in five groups**: zero-dimensional
`calloc`, aligned overflow and errno/output preservation, failed-reallocation ownership and retry,
`reallocarray` export/arithmetic/content, and zero-size ownership.
The baseline fails the original four groups. ASan independently identifies a call to
`malloc_usable_size` on the allocation freed by the old `Realloc` after its
replacement allocation failed. The baseline overflow checks return success
where failure is required; its `reallocarray` lookup is absent; its aligned
zero-size return is non-null.
The additional `calloc` group fails both `(0, SIZE_MAX)` and `(SIZE_MAX, 0)`
against the pre-follow-up source, while ordinary nonzero overflow still fails
with `ENOMEM` after the fix.

This Linux environment does not permit LeakSanitizer to inspect `/proc/*/task`.
The recorded local comparison therefore uses `ASAN_OPTIONS=detect_leaks=0`;
AddressSanitizer and UndefinedBehaviorSanitizer remain enabled. This is not a
leak-check result. No complete Windows runtime or translated ARM64 fixture was
executed locally, and the host test does not replace those integration suites.

## Native families still requiring independent work

The current source still explicitly treats `EPOLLET` as level-triggered
(`compat/posix/src/epoll.cc`). Socket ancillary transport remains absent;
the [sendmsg correction](native-socket-control-contracts-2026-11-08.md) rejects
control-bearing sends before transmitting their payload. The follow-up [system-adapter correction](native-system-contracts-2026-11-08.md)
implements `lockf`'s nonblocking range operations; its blocking `F_LOCK` mode
now reports `ENOSYS` because production `F_SETLKW` is still absent. Nested epoll, by contrast,
already has a concrete implementation and regressions; older documents that
describe all nested epoll as missing must not be repeated as current evidence.

Public-library handles are conditional on a named implemented probe in
`PlatformLibraryHandle`; listing a soname is not evidence that every export or
behavior exists. Guest-generated executable code, complete signal contexts,
graphics/media backends and allocator metadata exhaustion are outside the
checks above. These boundaries remain visible in the central compatibility
map and need distinct behavioral tests, not successful symbol lookup alone.
