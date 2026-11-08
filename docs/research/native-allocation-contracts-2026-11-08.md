# Native allocation compatibility audit, 2026-11-08

The original reviewed baseline is `a4dd7242cfed1a08cd8761ae896d63faf7b8cf63`.
The alignment follow-up below compares published
`223d4b3b6a4947bd3c1b3c679d95fc944b4d956a` with the additional correction.
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

References are Android's pinned [malloc API declarations](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/include/malloc.h),
[malloc regression tests](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/tests/malloc_test.cpp)
(`memalign_overflow`, `realloc_overflow`, `realloc_0`,
`realloc_nullptr_0`, `reallocarray_overflow`) and
[bionic API history](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/docs/status.md).

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

The initial correction passed **35 assertions in five groups**: zero-dimensional
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

## Alignment follow-up against published `223d4b3b`

`memalign`, `aligned_alloc`, and `posix_memalign` have different Android input
contracts. Android intentionally permits zero and arbitrary non-power-of-two
`memalign` alignments, rounding the latter up to the next power of two.
Rejecting all those inputs with `EINVAL` would break compatibility. However,
the published shim reduced them all to 16, so `memalign(33, size)` could return
storage lacking Android's 64-byte alignment. The maintained bionic
`malloc.memalign_non_power2` test requires success for every alignment from
zero through 256; that acceptance test alone does not prove the rounding.

The pinned default Scudo implementation explicitly performs this rounding in
[`wrappers_c.inc`](https://android.googlesource.com/platform/external/scudo/+/android-16.0.0_r4/standalone/wrappers_c.inc).
Android's supported low-memory jemalloc configuration applies the same policy
in [`jemalloc_wrapper.cpp`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/libc/bionic/jemalloc_wrapper.cpp).
The correction preserves zero as an allocation request with minimum alignment
and rounds other non-powers upward before calling the existing real allocation
backend. Its tracked ownership continues to support the exported `free`,
`realloc`, and `malloc_usable_size` paths.

`aligned_alloc` instead requires a nonzero power-of-two alignment and a size
divisible by that alignment. Its valid alignments start at one; the
`sizeof(void*)` minimum belongs to `posix_memalign` and must not be imposed
here. The published export incorrectly pointed to `Memalign`, returning
successful allocations for invalid `(alignment, size)` pairs such as `(3,3)`
and `(16,8)`. A separate C11 entry now validates these arguments, reports
`nullptr`/`EINVAL` on invalid inputs, and performs valid allocation through
the same storage backend. A size-multiple error is reported before an
allocation-overflow error.

This distinction is enforced by bionic's maintained
[`stdlib_test.cpp`](https://android.googlesource.com/platform/bionic/+/android-16.0.0_r4/tests/stdlib_test.cpp)
(`aligned_alloc_sweep`, `aligned_alloc_size_not_multiple_of_alignment`, and
`posix_memalign_sweep`) and Scudo's
[`wrappers_c_checks.h`](https://android.googlesource.com/platform/external/scudo/+/android-16.0.0_r4/standalone/wrappers_c_checks.h).
Scudo also tests the successful storage and invalid-size paths in
[`wrappers_c_test.cpp`](https://android.googlesource.com/platform/external/scudo/+/android-16.0.0_r4/standalone/tests/wrappers_c_test.cpp).

One deliberate safety qualification concerns an alignment above the highest
representable power of two. Pinned Scudo's `roundUpPowerOfTwo` in
[`common.h`](https://android.googlesource.com/platform/external/scudo/+/android-16.0.0_r4/standalone/common.h)
contains a word-width shift for that corner, so it provides no defined result
to reproduce. Gesher rejects an unrepresentable rounded alignment with
`ENOMEM` before shifting or allocating. This guard prevents successful
under-alignment; it is not presented as an experimentally established Android
errno for undefined upstream arithmetic. No Scudo-specific maximum alignment
or heap-capacity limit is imposed on Gesher's existing backend.

### Evidence and scope

The final actual-shim host run passes **79 assertions in seven groups** under
AddressSanitizer and UndefinedBehaviorSanitizer: the original 35 plus 25
`memalign` and 19 `aligned_alloc` assertions. The new tests cover the 0–256
acceptance sweep, actual rounded pointer alignment, simultaneous differently
sized allocations, alignments through 65536, writable bytes, usable size,
freeability, valid zero-size allocation, representability and size overflow,
every non-power `aligned_alloc` input through 2048, and valid small powers.
These assertions call the production exported symbols. The public
`aligned_alloc` entry is obtained by `dlsym`, so the existing API-26 native
fixture can test the API-28 export without changing its compilation floor.
The published volatile imports remain in place to prevent the compiler from
removing real allocator calls.

Running the same updated harness against published `223d4b3b` fails exactly
the two new groups; all five original groups still pass. An initial unsanitized
probe of the actual published implementation also returned 323 incorrectly
aligned pointers across 384 held allocations for boundaries 17, 33, 65, 129,
4097, and 32769, and accepted all six selected invalid C11 requests. Those
pointer counts describe that observed host run, not an allocator-independent
probability claim.

The complete updated `compat/bionic/shim.cc` compiles with the production
global definitions and strict warnings for both Windows x64 and native ARM64.
These object compilation checks and the host execution do not establish
Windows heap or translated ARM64 runtime behavior. The shared assertions are
included by the existing `tests/native/libc_test.c`, so both normal native
Windows/ELF fixtures will execute them in CI; no test or artifact gate was
removed. At this follow-up's source freeze, that new Windows execution is
still pending.

Downloaded reference SHA-256 values:

| Pinned source | SHA-256 |
| --- | --- |
| bionic `tests/malloc_test.cpp` | `ac9b10ab3ae16eba93d1b87da723e883cb72365e45253661280632416a50d496` |
| bionic `tests/stdlib_test.cpp` | `de763a04b20ae7b657a6da9fe5bef08b41bd0e793cec1b7e117794affb8a6c6b` |
| bionic `libc/bionic/jemalloc_wrapper.cpp` | `e8dfad9265609f5fbc3e625c7fdb0896ecb9cf78442cce594bf756e3057188ac` |
| Scudo `standalone/wrappers_c.inc` | `bc38e8a989305472f6e50151b19d28aac1975480580cea555ad5a30902309069` |
| Scudo `standalone/wrappers_c_checks.h` | `9ab7001fd692b84d83a763df12bec4b3d50b8c3a56f7ad710810ccd66eb08e27` |
| Scudo `standalone/common.h` | `81af9a132569acab4d53785111633e4d5830fc5211c814ada5d71d7dc641dd4e` |
| Scudo `standalone/tests/wrappers_c_test.cpp` | `2faec61e1acc808de9d10e99cd241e96bcb3ee67a5f5b47538a67c06235b446f` |

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
