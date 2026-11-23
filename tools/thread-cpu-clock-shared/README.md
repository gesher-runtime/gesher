# N28: shared host pthread CPU clock component

This is the first implementation stage for issue #56 N28. It builds a usable
shared winpthread supplier and two independent Windows consumers. It does not
replace the product's compiler, change its default provider, or claim N28 is
closed. `compat/posix/src/pthread.cc` still needs a separately reviewed adoption
change after component qualification.

## Why a supplier stage is necessary

The host adapter ignores the requested `pthread_t` and returns the caller's
fixed clock. Borrowing `pthread_gethandle` introduces a lifetime race: the
supplier unlocks before returning it, while join/detach/cancellation/native
exit can close it. A later duplication can capture a reused object.
Previous prototypes also recorded identical `pthread_t` values identifying
different targets in separate static and DLL providers. Centralizing clock
IDs cannot disambiguate an already ambiguous pthread ID.

This component uses one DLL for the pthread namespace and clock registry.
Both consumers select the supplier's `dllimport` declarations and explicitly
link its import library. The pthread function addresses used to check provider
identity come from the resolved runtime import table. Actual linker
inputs reject static winpthread inputs, PE tables identify the APIs, and tests
require both images to resolve the same functions and caller identity.
It does not make arbitrary prebuilt static providers share a namespace.
Adoption must exclude that topology from supported host components or provide
a separately qualified mechanism. The whole-pthread routing prototype is not
enabled or copied into this component.

## Ownership and API behavior

Source is pinned to mingw-w64 `a3d93999ef0521681d45e445c39964a7af0f593f`,
selected by llvm-mingw 20261006. `inputs.json` binds source/compiler archives,
original source hashes, patch hash and resulting hashes. The existing
retained-handle lifecycle work and selected-minimum detached-retirement/join
fixes are reused, with all required upstream notices retained.

The original native thread handle belongs to the supplier record until locked
registry retirement. Its legacy field is a bookkeeping alias. Creation
publishes ownership before resume; external threads retain their self handle.
The private duplicate API holds the same lock as final close, unregister and
record reuse. Success returns an owned query/synchronize handle and a
nonreused generation; failures preserve both output arguments.

The thread's TLS slot retains its own record until the supplier's final DLL
thread-detach callback clears that slot. Publishing `ended` before native
termination does not release this ownership. A concurrent `pthread_detach`
marks the record detached without blocking or pooling a record with pending
TLS use. The final callback reclaims detached records; a later detach may
reclaim a record after that callback. POSIX-created joinable records remain
for join, whose native wait includes final thread callbacks. Implicit native
attachments auto-retire on both ordinary Win32 return and `pthread_exit`,
including their default joinable state; no pthread join is required. The
supplier reclaims synchronization handles without repeating completed key cleanup.

Public `pthread_getcpuclockid` and time32/time64 readers live in the provider,
including literal exported symbols. A 256-slot table deduplicates generations
and reaps signaled objects. Explicit supplier DLL unload releases any remaining
clock handles after callers have stopped using the library. Reads use
`GetThreadTimes` on owned handles, never
reopen a TID. Dead/unknown pthreads return `ESRCH`; capacity/ID exhaustion returns
`EAGAIN`; retired clock reads return `EINVAL`. The pthread API preserves errno;
failure outputs stay unchanged. A live null-output read returns `EFAULT`.

Opaque host clock IDs are negative, monotonic and nonrecycling, with Linux's
thread/SCHED low-bit tag. They do not encode a reusable Windows TID and are not
Linux raw-syscall clock IDs. They must not cross into the separate Bionic guest
namespace. Bionic/syscall dispatch is unchanged. Fixed clocks keep their
existing implementation. Dynamic resolution uses the same Windows system
increment as the fixed CPU clock: FILETIME's 100 ns units are not precision.

## Reproduce

```sh
python3 tools/thread-cpu-clock-shared/component.py build \
  --llvm /path/to/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64 \
  --compiler-archive /path/to/compiler.tar.xz \
  --source-archive /path/to/mingw-w64-source.tar.gz \
  --output /path/to/fresh-build-directory
```

Source archive:
`https://codeload.github.com/mingw-w64/mingw-w64/tar.gz/a3d93999ef0521681d45e445c39964a7af0f593f`.
Compiler archive:
`https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz`.

The verified publisher compiler is read-only. Its actual regular-file hashes
and symlinks are checked against the archive. Each ABI receives a private full
supplier build, shared-only, with warnings as errors. The fixture includes the
upstream license. Manifests bind clean/dirty source, commit, run/attempt,
actual commands/compiler identities, input/patch identity and member hashes.

The scoped workflow builds both Windows ABIs, runs the Linux reference, and
executes x86-64 on Windows Server 2022 and 2025. Retry the complete workflow:
an older producer attempt cannot qualify a new consumer attempt. Local dirty
builds are labeled and cannot qualify CI.

## Runtime acceptance

- Eight real supplier load/unload cycles in an executable without pthread
  imports. Each cycle acquires a clock on a native thread and unloads after
  that thread exits, without issuing another clock call to reap the slot.
  The process handle count must return to its initial value.
- Busy and idle peers while the caller sleeps, measured against independent
  Win32 handle brackets; 10,000 acquisitions report cost and handle counts.
- Two-image public calls, function pointers, literal dynamic symbols,
  time32/time64, resolution and null/error output behavior.
- Explicit pthread, CreateThread, native pthread_exit(259), and std::thread
  targets; exit status 259 is not mistaken for a live object.
- A controlled later-loaded DLL thread-detach callback holds a target after
  its wrapper publishes `ended` and before supplier TLS cleanup. Detach must
  return while that target is gated; a pre-created, initially unattached
  native observer then allocates a record. Checks require distinct records,
  unchanged identity and CPU readings, once-only key cleanup, and eventual
  retirement. The gate creates no threads/modules while holding the loader
  lock; it exists only in the test DLL, not in the supplier implementation.
- 64 repetitions of each native return/`pthread_exit` and ordinary/detached
  combination: 256 measured exits after four warm-up exits. Every individual
  cycle must return to the exact handle baseline, run its key destructor once,
  and remove its registry entry. This check has no per-cycle growth allowance.
- Owned private capability survival after retirement and failure outputs.
- 256 live clocks and a real 257th target, cached success at capacity,
  exit reaping and 64 generations without saved-clock aliasing.
- Four readers across 32 exit/join/detach races, real cancellation,
  128 detached creations, peer unload and bounded handle reclamation.

The Linux reference checks the busy peer, literal libc symbols, encoded target
identity and 64 final kernel releases. It uses tgkill and clock rejection after
join, avoiding disappearing /proc reads and invalid post-join pthread_t access.
It is a Linux reference, not Android-device, Windows, ART or guest evidence.

## Before product adoption

After independent QA and actual Windows component results, update every
supported host producer to the same supplier, ship/verify its DLL and notices,
and remove the incorrect host stub without introducing another namespace.
Rebuild host, translator and dependent components together; retain fixed-clock,
Bionic/guest, ART/APK, installed-package and e2e gates. Native Windows ARM64
execution, deterministic resource-failure injection, mixed static-provider
topologies and full product qualification remain separate work.

References: [issue #56](https://github.com/gesher-runtime/gesher/issues/56),
[pinned supplier lifecycle](https://github.com/mingw-w64/mingw-w64/blob/a3d93999ef0521681d45e445c39964a7af0f593f/mingw-w64-libraries/winpthreads/src/thread.c),
[GetThreadTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes),
[Android pthread API](https://developer.android.com/ndk/reference/group/threading#pthread_getcpuclockid).
Historical evidence remains in `docs/handoff/tracks/host-pthread-authority.md`
and its adoption plan.
