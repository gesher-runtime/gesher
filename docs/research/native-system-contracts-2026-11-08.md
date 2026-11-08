# Native system adapter contracts, 2026-11-08

This audit follows the [native allocation correction](native-allocation-contracts-2026-11-08.md).
It reviews `Getgroups` and `Lockf` in `compat/bionic/extra_sys_bionic.cc`
against baseline `a4dd7242cfed1a08cd8761ae896d63faf7b8cf63`.

## `getgroups`: validate before touching caller memory

The synthetic group implementation previously accepted a negative count and
unconditionally wrote `list[0]` for any nonzero count. A positive count and null
output crashed; a negative count returned false success and modified the output.

Negative counts now return `-1/EINVAL`. The valid `(0, nullptr)` size query
still reports one group. Positive counts with null output return `-1/EFAULT`.
The successful one-group result and bounded output remain unchanged. This does
not implement Windows supplementary-group mapping or guard arbitrary invalid
non-null pointers.

## `lockf`: real ranges for supported commands

The old adapter checked only that the file descriptor existed, then reported
success for every `F_ULOCK`, `F_LOCK`, `F_TLOCK` and `F_TEST` call. No lock was
acquired or released and no conflict was examined. This could defeat an app's
file or database coordination while giving it no error to handle.

The adapter now routes `F_TLOCK` and `F_ULOCK` through production `F_SETLK`;
it preserves the current-offset origin, signed 64-bit length, negative ranges,
and zero-length-through-EOF semantics. `F_TEST` uses `F_GETLK` and bionic's
read-lock query, returning `EACCES` for a foreign writer. Both `lockf` and
`lockf64` retain the shared LP64 export. The reference is
[AOSP's lockf implementation](https://android.googlesource.com/platform/bionic/+/main/libc/bionic/lockf.cpp)
and its [public contract](https://android.googlesource.com/platform/bionic/+/main/libc/include/bits/lockf.h).

**Blocking `F_LOCK` remains unsupported.** It validates the descriptor and
returns `-1/ENOSYS`, without publishing ownership. The current production
`fcntl` dispatch handles `F_GETLK`/`F_SETLK` and their OFD equivalents, but not
`F_SETLKW`. The separate bounded `WaitProcessLock` component is not a complete
production blocking/signal/descriptor-lifetime implementation and is not wired
into this adapter. No sleep/retry loop or false blocking success is introduced.

## Verification and boundaries

`tests/native/system_contract_test.inc` is included in the normal x86-64 and
translated ARM64 ELF libc suites. It uses a separately opened file description
and real OFD `fcntl` operations to witness range ownership and conflicts.
The witness can distinguish an actual lock from a function that merely returns
zero. Cases cover partial unlock, foreign owners, shared locks, negative
ranges, read-only descriptors, invalid commands, offsets above 32 bits,
through-EOF locking and truthful blocking rejection.

```sh
python3 tools/test_bionic_system_contracts.py --sanitize
```

The lightweight Linux envelope compiles the two production functions and their
public registrations verbatim, then uses the host kernel as the independent
`fcntl` backend. **41 assertions pass** with ASan/UBSan enabled. The old source
fails both groups: UBSan reports the null `getgroups` store, and 10 `lockf`
ownership/error checks fail. The tests and both shared C fixtures also pass
compiler syntax/type checking with warnings treated as errors.

This executor requires `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer
cannot inspect its `/proc/*/task`; address and undefined-behavior checking remain
enabled. A complete Windows build and the actual Windows descriptor registry
were not executed locally. The ELF tests are the integration gate for that
backend; this host result establishes the adapter, not cross-platform lock
lifetime conformance or complete Android compatibility.
